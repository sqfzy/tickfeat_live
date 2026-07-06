#!/usr/bin/env python3
# diff_factors.py — tickfeat_live --dump CSV vs 离线黄金参照 parquet 逐位对拍(全 27 币 × 11 列)。
# 铁律: read_csv 必须 float_precision="round_trip"(否则测量侧自带 1 ULP 噪声)。
# 用法: diff_factors.py <dump.csv> <ref_dir>   (ref_dir 含 ref_<SYM>.parquet)
import sys, os
import pandas as pd

REC, REFDIR = sys.argv[1], sys.argv[2]
GID = ["AAVEUSDT","ADAUSDT","APTUSDT","ARBUSDT","AVAXUSDT","AXSUSDT","BCHUSDT","BNBUSDT","CRVUSDT",
       "DOGEUSDT","DOTUSDT","ENSUSDT","HBARUSDT","LTCUSDT","OPUSDT","ORDIUSDT","PEOPLEUSDT","PNUTUSDT",
       "SANDUSDT","SOLUSDT","SUIUSDT","TIAUSDT","TRUMPUSDT","UNIUSDT","WLDUSDT","XLMUSDT","XRPUSDT"]
COLS = ["f0","f1","f2","f3","f4","f5","f6","f7","f8","f9","mid"]  # dump 的 mid ↔ 参照 mid_price

rec = pd.read_csv(REC, float_precision="round_trip")
gmax = {c: 0.0 for c in COLS}
gargmax = {c: None for c in COLS}
tot_matched = 0
per_lid, missing_ref = [], []
for lid in sorted(rec["lid"].unique()):
    sym = GID[int(lid)]
    refp = os.path.join(REFDIR, f"ref_{sym}.parquet")
    if not os.path.exists(refp):
        missing_ref.append(sym); continue
    ref = pd.read_parquet(refp).rename(columns={"mid_price": "mid"})
    r = rec[rec["lid"] == lid]
    m = r.merge(ref[["ts_us"] + COLS], on="ts_us", suffixes=("_on", "_off"), how="inner")
    per_lid.append((sym, len(r), len(ref), len(m)))
    tot_matched += len(m)
    for c in COLS:
        d = (m[f"{c}_on"] - m[f"{c}_off"]).abs()
        both_nan = m[f"{c}_on"].isna() & m[f"{c}_off"].isna()   # 暖机列两边同 NaN=一致
        d = d[~both_nan]
        if len(d) and d.max() > gmax[c]:
            gmax[c] = float(d.max()); gargmax[c] = (sym, int(m.loc[d.idxmax(), "ts_us"]))

print(f"dump 行={len(rec)}  匹配行={tot_matched}  币数={rec['lid'].nunique()}")
if missing_ref:
    print("缺参照:", missing_ref)
print("\n=== 全局 max|diff| 逐列 ===")
allzero = True
for c in COLS:
    if gmax[c] != 0.0: allzero = False
    loc = f"  @ {gargmax[c]}" if gargmax[c] else ""
    print(f"  {c:5s} max|diff| = {gmax[c]:.3e}  [{'OK' if gmax[c]==0.0 else '≠0'}]{loc}")
print("\n结论:", "✅ 全 11 列 max|diff| = 0(bit-exact)" if allzero else "❌ 存在非零残差")
sys.exit(0 if allzero else 1)
