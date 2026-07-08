#!/usr/bin/env python3
"""self_recompute_diff.py — tickfeat_live dump 的「因子从自己内嵌 raw 完美复算」自校验(DepthBoard 口径)。

取 dump(JSONL, 每行 {lid,ts_us,factors,source_raw:{ob,tr,bn}}), 用每行内嵌的 source_raw 桥接成
formatted, 跑黄金参照 build_tick_feat_standalone.compute_day, 逐位对回存的 f0-f9+mid。

设计要点(踩过的坑):
- 顺序逐币(不全量载内存, 曾一次载全 27 币 ~20GB OOM 崩机)。内存 = 单币峰值: 大币可 ~6-9GB
  (桥接的 66 列全零填充 formatted + JSON 对象膨胀所致)。曾试 multiprocessing 并行, 但 N 个大币 worker
  叠加轻松 >20GB, 反而抖动/近 OOM —— 故改回顺序, 稳。
- 时间维度不拆(cumsum/滚动因子 f3-f9 需从冷启动连续历史), 但按币拆保留每币完整时间线, f8/f9 照算。
- 每币数据写【单文件】(不按天拆) → depth as-of 可跨 UTC 午夜(否则 imb5 在午夜后取不到前一天末条 OB, 假告警)。
- 跳每币【首行】(冷启动 as-of 边界: 首行 imb/spread 的 as-of OB 在录制起点之前, 文件里没有, 天然缺一格)。
- 稀疏零洞: dump 被 copytruncate 压缩后前段是稀疏零洞, 用 SEEK_DATA 跳过。
- 稳定排序: 桥接同 ts 多条 OB 按 (ts,update_id) 升序写(=离线 kind=stable 取末条=最高 uid)。

用法: python3 self_recompute_diff.py <dump.jsonl> <CLO_us> <CHI_us>
环境: OFFLINE_DIR=<含 build_tick_feat_standalone.py 的目录> (默认 /root/tf_new/pipe);
      WORK_DIR=<临时工作目录> (默认 /root/tf_new/self_recompute_work)。
依赖: pandas pyarrow + build_tick_feat_standalone(DepthBoard 口径, 2 参 compute_day)。
"""
import json, os, sys, glob, shutil, datetime
import pandas as pd, pyarrow as pa, pyarrow.parquet as pq

OFFLINE_DIR = os.environ.get("OFFLINE_DIR", "/root/tf_new/pipe")
sys.path.insert(0, OFFLINE_DIR)
import build_tick_feat_standalone as B   # DepthBoard 口径: compute_day(okx_paths, bn_paths, target_date)

F   = sys.argv[1]
CLO = int(sys.argv[2])
CHI = int(sys.argv[3])
SYMBOLS = ["AAVEUSDT","ADAUSDT","APTUSDT","ARBUSDT","AVAXUSDT","AXSUSDT","BCHUSDT","BNBUSDT",
           "CRVUSDT","DOGEUSDT","DOTUSDT","ENSUSDT","HBARUSDT","LTCUSDT","OPUSDT","ORDIUSDT",
           "PEOPLEUSDT","PNUTUSDT","SANDUSDT","SOLUSDT","SUIUSDT","TIAUSDT","TRUMPUSDT",
           "UNIUSDT","WLDUSDT","XLMUSDT","XRPUSDT"]
NL = 15
COLS = ["ts","side","price","amount"] + [f"bp{i}" for i in range(NL)] + [f"ba{i}" for i in range(NL)] \
       + [f"ap{i}" for i in range(NL)] + [f"aa{i}" for i in range(NL)] + ["trade_id","local_ns"]
COLMAP = [("f0","f0"),("f1","f1"),("f2","f2"),("f3","f3"),("f4","f4"),("f5","f5"),
          ("f6","f6"),("f7","f7"),("f8","f8"),("f9","f9"),("mid","mid_price")]
WORK = os.environ.get("WORK_DIR", "/root/tf_new/self_recompute_work")
DEMUX = WORK + "/demux"; FMT = WORK + "/fmt"
FDATE = "20260101"   # 单文件统一日名(compute_day 按实际 ts 过滤网格, 名义日无所谓)


def dateof(ts_us): return datetime.datetime.utcfromtimestamp(ts_us / 1e6).strftime("%Y%m%d")
def open_dump(path):
    fd = os.open(path, os.O_RDONLY)
    try: os.lseek(fd, 0, os.SEEK_DATA)   # 跳 copytruncate 稀疏零洞
    except OSError: pass
    return os.fdopen(fd, "rb")
def blank(): return [0] * len(COLS)
def ob_row(ts, bpx, apx, bsz, asz):
    r = blank(); r[0] = ts; r[2] = 0; r[4] = bpx; r[4 + 2 * NL] = apx
    for i in range(len(bsz)): r[4 + NL + i] = bsz[i]
    for i in range(len(asz)): r[4 + 3 * NL + i] = asz[i]
    return r
def tr_row(ts, side, px, amt, tid):
    r = blank(); r[0] = ts; r[1] = side; r[2] = px; r[3] = amt; r[64] = tid; return r
def neq(a, b):
    a = float("nan") if a is None else float(a); b = float("nan") if b is None else float(b)
    if (a != a) and (b != b): return 0.0
    if (a != a) or (b != b): return float("inf")
    return abs(a - b)
def write_single(rows, prefix, sym, outdir):
    if not rows: rows = [blank()]
    df = pd.DataFrame(rows, columns=COLS)
    pq.write_table(pa.Table.from_pandas(df, preserve_index=False), f"{outdir}/{prefix}_{sym}_{FDATE}.parquet")


def process_lid(lid):
    """worker: 一个币 —— 载 → 桥接(ob→okx_swap, tr→okx_swap, bn→binance_swap) → compute_day 逐日 → 跳首行 → diff。"""
    sym = SYMBOLS[lid]
    outdir = f"{FMT}/lid{lid}"; os.makedirs(outdir, exist_ok=True)
    ob = []; tr = []; bn = []; stored = {}
    for line in open(f"{DEMUX}/lid{lid}.jsonl", "rb"):
        d = json.loads(line); sr = d["source_raw"]
        ob += sr["ob"]; tr += sr["tr"]; bn += sr["bn"]
        stored[d["ts_us"]] = d["factors"]
    # okx_swap: OB 5 档(按 (ts,uid) 稳序) + 成交
    rows = [ob_row(e[0], e[1], e[2], e[3:8], e[8:13]) for e in sorted(ob, key=lambda e: (e[0], e[13]))]
    rows += [tr_row(e[0], e[1], e[2], e[3], e[5]) for e in tr]
    rows.sort(key=lambda r: r[0])
    write_single(rows, "okx_swap", sym, outdir)
    # binance_swap: BN BBO
    rows = []
    for e in sorted(bn, key=lambda e: (e[0], e[3])):
        r = ob_row(e[0], e[1], e[2], [], []); r[4] = e[1]; r[4 + 2 * NL] = e[2]; rows.append(r)
    write_single(rows, "binance_swap", sym, outdir)

    okx = sorted(glob.glob(f"{outdir}/okx_swap_{sym}_*.parquet"))
    bnp = sorted(glob.glob(f"{outdir}/binance_swap_{sym}_*.parquet"))
    first_ts = min(stored) if stored else None
    agg = {k: 0.0 for k, _ in COLMAP}; nd = {k: 0 for k, _ in COLMAP}; tot = 0
    for D in sorted(set(dateof(t) for t in stored)):
        try: ref = B.compute_day(okx, bnp, D)     # DepthBoard 口径: 2 参
        except Exception:
            continue
        if ref is None or ref.empty: continue
        rmap = {int(r.ts_us): r for r in ref.itertuples()}
        for t, fac in stored.items():
            if t == first_ts: continue            # 跳冷启动首行边界
            if dateof(t) != D or t not in rmap: continue
            tot += 1; rr = rmap[t]
            for sk, rk in COLMAP:
                dd = neq(fac[sk], getattr(rr, rk))
                if dd > agg[sk]: agg[sk] = dd
                if dd > 1e-9: nd[sk] += 1
    shutil.rmtree(outdir, ignore_errors=True)
    return (sym, agg, nd, tot)


def main():
    shutil.rmtree(WORK, ignore_errors=True); os.makedirs(DEMUX); os.makedirs(FMT)
    handles = {}; n = 0
    for line in open_dump(F):                      # Pass 1: demux(串行, 低内存)
        if line[:1] != b"{": continue
        try: d = json.loads(line)
        except Exception: continue
        t = d["ts_us"]
        if t < CLO or t >= CHI: continue
        lid = d["lid"]
        h = handles.get(lid)
        if h is None: h = handles[lid] = open(f"{DEMUX}/lid{lid}.jsonl", "wb")
        h.write(line); n += 1
    for h in handles.values(): h.close()
    lids = sorted(handles)
    print(f"demux 完: {n} 行 -> {len(lids)} 币; 顺序逐币(内存=单币峰值)", flush=True)

    AGG = {k: 0.0 for k, _ in COLMAP}; ND = {k: 0 for k, _ in COLMAP}; TOT = 0
    for lid in lids:                                # Pass 2: 顺序逐币(内存有界=单币, 每币算完即释放)
        sym, agg, nd, tot = process_lid(lid)
        TOT += tot
        for sk, _ in COLMAP:
            AGG[sk] = max(AGG[sk], agg[sk]); ND[sk] += nd[sk]
        print(f"  {sym} 完({tot} 秒, 累计 {TOT})", flush=True)

    print(f"\n对比秒数(共同, 已跳每币首行)={TOT}")
    print("列   max|diff|      有差秒")
    for sk, _ in COLMAP:
        print(f"{sk:4s} {AGG[sk]:.3e}   {ND[sk]}")


if __name__ == "__main__":
    main()
