#!/usr/bin/env bash
# verify_live.sh — tickfeat_live 端到端验证:对离线黄金参照 bit-exact(11 列) + 源血缘正确。
#
# 流程(函数即目录): by-venue 数据 → 离线参照 → mdreplay 配置 → 回放 + tickfeat_live --dump → 对拍 + 血缘核对。
# 用法: bash test/verify_live.sh          (环境变量可覆盖 SRC/DATE/REALTIME/WINDOW_MIN/WORK)
#       SKIP_GEN=1 SKIP_REF=1 bash ...     (复用上次的数据/参照, 只重跑回放+对拍)
#
# 铁律:
#  - 争用机器用 REALTIME=1.0(实时, 余量最大)。latest-wins 轮询在负载/高速下会非确定丢采 → 因子残差/血缘 hi 仍中但 lo 偏。
#  - 回放整文件不设 start(在线首秒=参照首秒), 只截末端窗口(截尾不影响 cumsum/pm)。WINDOW_MIN≥6 覆盖 f6/f7 的 300s。
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIVE_ROOT="$(cd "$HERE/.." && pwd)"
PROJ="$(cd "$LIVE_ROOT/.." && pwd)"

SRC="${SRC:-$PROJ/formatted_ec2_5m}"     # formatted 源(含 <venue>_swap_<SYM>_<DATE>.csv)
DATE="${DATE:-20260630}"
REALTIME="${REALTIME:-1.0}"
WINDOW_MIN="${WINDOW_MIN:-10}"
WORK="${WORK:-$HERE/.work}"              # 中间产物(gitignore)
MDR="$PROJ/mdreplay/build/linux/x86_64/release/mdreplay"
LIVE="$LIVE_ROOT/build/linux/x86_64/release/tickfeat_live"
STANDALONE="$PROJ/tick_feat_deliverable/build_tick_feat_standalone.py"
PY="uv run --no-project --with pandas --with pyarrow python3"
SYMS="AAVEUSDT ADAUSDT APTUSDT ARBUSDT AVAXUSDT AXSUSDT BCHUSDT BNBUSDT CRVUSDT DOGEUSDT DOTUSDT ENSUSDT HBARUSDT LTCUSDT OPUSDT ORDIUSDT PEOPLEUSDT PNUTUSDT SANDUSDT SOLUSDT SUIUSDT TIAUSDT TRUMPUSDT UNIUSDT WLDUSDT XLMUSDT XRPUSDT"

die(){ echo "verify_live: $*" >&2; exit 1; }
[ -x "$MDR" ]  || die "缺 mdreplay: $MDR (先 cd $PROJ/mdreplay && xmake build)"
[ -x "$LIVE" ] || die "缺 tickfeat_live: $LIVE (先 cd $LIVE_ROOT && xmake build)"

gen_data(){   # okx 走 5 档(imb5 需 L0-L4), bn 走 1 档(depth==1 → mdreplay 路由成 BookTickBoard)
  echo "[1/5] 生成 by-venue 数据(okx 5档 / bn 1档)"
  $PY "$PROJ/formatted_to_datas.py" --src "$SRC" --out "$WORK/datas5" --by-venue --depth 5 >/dev/null
  $PY "$PROJ/formatted_to_datas.py" --src "$SRC" --out "$WORK/datas1" --by-venue --depth 1 >/dev/null
}

build_refs(){
  echo "[2/5] 建离线黄金参照(27 币, warmup_days=0)"
  mkdir -p "$WORK/refs"
  for s in $SYMS; do
    $PY "$STANDALONE" --raw_dir "$SRC" --symbol "$s" --date "$DATE" --warmup_days 0 \
      --out "$WORK/refs/ref_$s.parquet" >/dev/null 2>&1 || echo "  ! $s 参照失败"
  done
}

write_config(){   # 计算 init_ts(首事件前 ~5s 墙钟 attach 窗口) + 窗口末端, 写 mdreplay toml
  echo "[3/5] 写 mdreplay 配置"
  $PY - "$WORK/datas5/okx" "$WORK/datas1/binance" "$REALTIME" "$WINDOW_MIN" "$WORK/config.toml" <<'PY'
import sys, glob, os, datetime
okx, bn, rt, win, out = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
mn = None
for d, pat in ((okx, "*.book.csv"), (okx, "*.trade.csv"), (bn, "*.book.csv")):
    for f in glob.glob(os.path.join(d, pat)):
        with open(f) as fh:
            fh.readline(); l = fh.readline()
            if l.strip(): t = int(l.split(",", 1)[0]); mn = t if mn is None else min(mn, t)
init_ts = mn - int(5e9 / rt)                       # realtime 越小回放越快 → grace 数据窗需越大
end = datetime.datetime.utcfromtimestamp((mn + win * 60 * 10**9) / 1e9).strftime("%Y-%m-%d %H:%M:%S")
rep = lambda d, k, seg: f'[[replays]]\ninput  = {{ format = "csv", dir = "{d}", kind = "{k}" }}\noutput = {{ path = "{seg}", create = true }}\nend    = "{end}"\n'
open(out, "w").write(
    f"realtime = {rt}\ninit_ts = {init_ts}\n"
    + rep(okx, "book", "/shm_okx_depth") + rep(okx, "trade", "/shm_okx_trade") + rep(bn, "book", "/shm_bn_booktick")
    + '[log]\nlevel = "info"\nprogress_sec = 30\n')
print(f"  init_ts={init_ts} end={end} realtime={rt}")
PY
}

run_replay(){   # mdreplay 建段(init_ts 延迟) → consumer attach → 回放 → 收尾(host 退出前 flush dump)
  echo "[4/5] mdreplay 回放 + tickfeat_live --dump"
  for s in shm_okx_depth shm_okx_trade shm_bn_booktick shm_tickfeat_out; do rm -f "/dev/shm/$s"; done
  rm -f "$WORK/live_dump.csv"
  "$MDR" --config "$WORK/config.toml" > "$WORK/mdr.log" 2>&1 &  local mdr=$!
  for i in $(seq 1 80); do [ -e /dev/shm/shm_okx_depth ] && [ -e /dev/shm/shm_bn_booktick ] && break; sleep 0.1; done
  [ -e /dev/shm/shm_okx_depth ] || { cat "$WORK/mdr.log"; kill $mdr 2>/dev/null; die "段未建成"; }
  "$LIVE" /shm_okx_depth /shm_okx_trade /shm_bn_booktick /shm_tickfeat_out -1 200 \
      --dump "$WORK/live_dump.csv" > "$WORK/live.log" 2>&1 &  local live=$!
  wait $mdr
  sleep 3
  kill -TERM $live 2>/dev/null; wait $live 2>/dev/null
  echo "  dump: $(( $(wc -l < "$WORK/live_dump.csv") - 1 )) 行"
}

verify(){
  echo "[5/5] 对拍(11 列 bit-exact) + 源血缘核对"
  $PY "$HERE/diff_factors.py"  "$WORK/live_dump.csv" "$WORK/refs"                          || FAIL=1
  $PY "$HERE/check_lineage.py" "$WORK/live_dump.csv" "$WORK/datas5/okx" "$WORK/datas1/binance" || FAIL=1
}

mkdir -p "$WORK"; FAIL=0
[ "${SKIP_GEN:-0}" = 1 ] || gen_data
[ "${SKIP_REF:-0}" = 1 ] || build_refs
write_config
run_replay
verify
[ "$FAIL" = 0 ] && echo "✅ verify_live 通过(11 列 max|diff|=0 + 血缘正确)" || { echo "❌ verify_live 失败"; exit 1; }
