#!/usr/bin/env python3
# check_lineage.py — 校验 dump 的 6 列源血缘 == 源该秒实际 id 区间(每币抽查前 40 行)。
# dump 记的是引擎【消费到】的 id;无损回放(realtime=1.0 无争用)下 = 源【产生】的 → 应 0 不符。
# 有争用/高速下 latest-wins 轮询会丢采 → 消费 ⊆ 源(桶末 hi 仍中),此时少量不符属预期(非 bug)。
# 用法: check_lineage.py <dump.csv> <okx_dir> <bn_dir>   (okx_dir 含 book+trade csv, bn_dir 含 book csv)
import sys, pandas as pd

DUMP, OKX, BN = sys.argv[1], sys.argv[2], sys.argv[3]
GID = ["AAVEUSDT","ADAUSDT","APTUSDT","ARBUSDT","AVAXUSDT","AXSUSDT","BCHUSDT","BNBUSDT","CRVUSDT",
       "DOGEUSDT","DOTUSDT","ENSUSDT","HBARUSDT","LTCUSDT","OPUSDT","ORDIUSDT","PEOPLEUSDT","PNUTUSDT",
       "SANDUSDT","SOLUSDT","SUIUSDT","TIAUSDT","TRUMPUSDT","UNIUSDT","WLDUSDT","XLMUSDT","XRPUSDT"]
LNS = ["ob_uid_lo","ob_uid_hi","bn_uid_lo","bn_uid_hi","tr_ns_lo","tr_ns_hi","tr_id_lo","tr_id_hi"]

d = pd.read_csv(DUMP, dtype={c: "int64" for c in LNS + ["lid", "ts_us"]})  # 强制 int64,避免大 ns 落 float

def load(p):
    x = pd.read_csv(p); x["ts_us"] = x.ts // 1000; x["buck"] = (x.ts_us // 1_000_000) * 1_000_000; return x

mism = checked = 0
for lid in sorted(d.lid.unique()):
    sym = GID[int(lid)]
    ob = load(f"{OKX}/{sym}.book.csv"); bn = load(f"{BN}/{sym}.book.csv"); tr = load(f"{OKX}/{sym}.trade.csv")
    for r in d[d.lid == lid].head(40).itertuples(index=False):   # itertuples 保各列 dtype,不落 float
        q = int(r.ts_us); checked += 1
        o, b, t = ob[ob.buck == q], bn[bn.buck == q], tr[tr.buck == q]
        exp = ((int(o.update_id.min()), int(o.update_id.max())) if len(o) else (-1, -1)) \
            + ((int(b.update_id.min()), int(b.update_id.max())) if len(b) else (-1, -1)) \
            + ((int(t.ts.min()), int(t.ts.max())) if len(t) else (-1, -1)) \
            + ((int(t.trade_id.min()), int(t.trade_id.max())) if len(t) else (-1, -1))
        got = (int(r.ob_uid_lo), int(r.ob_uid_hi), int(r.bn_uid_lo), int(r.bn_uid_hi),
               int(r.tr_ns_lo), int(r.tr_ns_hi), int(r.tr_id_lo), int(r.tr_id_hi))
        if got != exp:
            mism += 1
            if mism <= 8: print(f"  ✗ {sym} ts={q}\n     got={got}\n     exp={exp}")

print(f"\n抽查 {checked} 行(每币前 40): 不匹配 {mism}")
print("✅ 血缘全对: dump 6 列 == 源该秒实际 id 区间" if mism == 0 else "❌ 见上(高速/争用下少量不符=轮询丢采,非 bug)")
sys.exit(0 if mism == 0 else 1)
