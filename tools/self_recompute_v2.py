#!/usr/bin/env python3
"""self_recompute_v2.py — 全 13 列自复算 diff: f0-f9+mid(build_tick_feat_standalone)
+ pdiff_v2/mean_v2(factor_calc_v2_new 整数口径, 从内嵌 raw 重算)。
pdiff_v2 = (bn_mid_i - okx_mid_i)*1e9/okx_mid_i (int64 向零截断); okx_mid_i=(桶末OB bid+ask)//2,
bn_mid_i=(as-of BN bid+ask)//2 (均 ×1e8 整数); mean_v2 = (int64)((Σpdiff_v2/Σpcnt/1e9)*1e9),
窗口 12h(43200), Σpcnt≥10800 才出否则 0 —— 逐字复刻 time_ring.h + main.cpp。
用法: python3 self_recompute_v2.py <dump.jsonl> <CLO_us> <CHI_us>
"""
import json, os, sys, glob, shutil, datetime, bisect
import numpy as np, pandas as pd, pyarrow as pa, pyarrow.parquet as pq

OFFLINE_DIR = os.environ.get("OFFLINE_DIR", "/root/tf_new/pipe")
sys.path.insert(0, OFFLINE_DIR)
import build_tick_feat_standalone as B

F = sys.argv[1]; CLO = int(sys.argv[2]); CHI = int(sys.argv[3])
DFROM = int(sys.argv[4]) if len(sys.argv) > 4 else CLO   # 只对 t>=DFROM 的行计 diff(暖机区不计); 暖机仍用全窗
# 接缝剔除: 若因子的窗口 [q-W, q] 覆盖 [SEAM_LO, SEAM_HI](旧 compress 丢行区), 该因子该秒不计 diff。
# SEAM env: "lo1-hi1,lo2-hi2"(ts_us)。W 按因子(秒)。
SEAMS = []
for seg in os.environ.get("SEAM", "").split(","):
    if "-" in seg:
        a, b = seg.split("-"); SEAMS.append((int(a) // 1000000, int(b) // 1000000))
DBG = set(os.environ.get("DBG_DIFF", "").split(",")) - {""}   # 要逐秒打印有差秒的列名
WIN = {"f0": 2, "f1": 2, "f2": 2, "f3": 60, "f4": 10, "f5": 60, "f6": 300,
       "f7": 300, "f8": 7200, "f9": 43200, "mid": 1, "pdiff_v2": 1, "mean_v2": 43200}
def seam_hit(col, q_us):
    qs = q_us // 1000000; w = WIN[col]
    for lo, hi in SEAMS:
        if qs >= lo and qs - w <= hi: return True   # 窗口 [q-w, q] 与 [lo,hi] 相交
    return False
SYMBOLS = ["AAVEUSDT","ADAUSDT","APTUSDT","ARBUSDT","AVAXUSDT","AXSUSDT","BCHUSDT","BNBUSDT",
           "CRVUSDT","DOGEUSDT","DOTUSDT","ENSUSDT","HBARUSDT","LTCUSDT","OPUSDT","ORDIUSDT",
           "PEOPLEUSDT","PNUTUSDT","SANDUSDT","SOLUSDT","SUIUSDT","TIAUSDT","TRUMPUSDT",
           "UNIUSDT","WLDUSDT","XLMUSDT","XRPUSDT"]
NL = 15; GRID = 1_000_000; W12H = 43200 * GRID
COLS = ["ts","side","price","amount"] + [f"bp{i}" for i in range(NL)] + [f"ba{i}" for i in range(NL)] \
       + [f"ap{i}" for i in range(NL)] + [f"aa{i}" for i in range(NL)] + ["trade_id","local_ns"]
COLMAP = [("f0","f0"),("f1","f1"),("f2","f2"),("f3","f3"),("f4","f4"),("f5","f5"),
          ("f6","f6"),("f7","f7"),("f8","f8"),("f9","f9"),("mid","mid_price")]
V2COLS = ["pdiff_v2","mean_v2"]
WORK = os.environ.get("WORK_DIR", "/root/tf_new/srv2_work")
DEMUX = WORK + "/demux"; FMT = WORK + "/fmt"; FDATE = "20260101"


def dateof(t): return datetime.datetime.utcfromtimestamp(t / 1e6).strftime("%Y%m%d")
def open_dump(p):
    fd = os.open(p, os.O_RDONLY)
    try: os.lseek(fd, 0, os.SEEK_DATA)
    except OSError: pass
    return os.fdopen(fd, "rb")
def neq(a, b):
    a = float("nan") if a is None else float(a); b = float("nan") if b is None else float(b)
    if a != a and b != b: return 0.0
    if a != a or b != b: return float("inf")
    return abs(a - b)

# 逐列 numpy 预分配(替代"每事件一个 66 元素 Python list")。
# 原实现每事件 ~2.4KB(list 开销 + 66 个 int 对象), 数百万事件即 6-7GB, 再经 pd.DataFrame
# 对象推断翻倍 → 18GB OOM(单次 diff 卡在 ~5 万秒)。逐列 numpy 降到 n×528B, 峰值降 ~6 倍。
# 量(amount/ba*/aa*)float64、价/id/ts int64 —— 与原 pandas 逐列推断的有效 dtype 一致。
def build_cols(n):
    return {c: (np.zeros(n, np.float64) if (c == "amount" or c[:2] in ("ba", "aa")) else np.zeros(n, np.int64))
            for c in COLS}

def write_cols(cols, prefix, sym, outdir):
    pq.write_table(pa.Table.from_pandas(pd.DataFrame(cols, columns=COLS), preserve_index=False),
                   f"{outdir}/{prefix}_{sym}_{FDATE}.parquet")
def itrunc(n, d):   # int 向零截断除 (C++ / 语义), d>0
    return n // d if n >= 0 else -((-n) // d)


def recompute_v2(ob_sorted, bn_sorted, secs):
    """factor_calc_v2_new 整数口径复算 pdiff_v2/mean_v2。secs=已结算秒(升序)。返回 {q:(pdiff_v2|nan, mean_v2)}。"""
    okx_mid = {}                                        # 桶末整数 mid: 按 (ts,uid) 序末条覆盖 = bucket-end
    for e in ob_sorted: okx_mid[(e[0] // GRID) * GRID] = (e[1] + e[2]) // 2
    bn_mid = {}
    for e in bn_sorted: bn_mid[(e[0] // GRID) * GRID] = (e[1] + e[2]) // 2
    bn_secs = sorted(bn_mid)
    pv = np.empty(len(secs), np.float64); pc = np.empty(len(secs), np.float64)
    pvraw = [0.0] * len(secs)
    for i, q in enumerate(secs):
        om = okx_mid.get(q, 0)
        j = bisect.bisect_right(bn_secs, q) - 1
        bm = bn_mid[bn_secs[j]] if j >= 0 else 0
        if om > 0 and bm > 0:
            v = itrunc((bm - om) * 1_000_000_000, om)
            pv[i] = float(v); pc[i] = 1.0; pvraw[i] = float(v)
        else:
            pv[i] = 0.0; pc[i] = 0.0; pvraw[i] = float("nan")
    cs_pv = np.cumsum(pv); cs_pc = np.cumsum(pc)
    ts_arr = np.array(secs, np.int64)
    out = {}
    for i, q in enumerate(secs):
        ib = bisect.bisect_right(ts_arr, q - W12H - 1) - 1     # 窗口 (q-12h-1, q]
        s = cs_pv[i] - (cs_pv[ib] if ib >= 0 else 0.0)
        c = cs_pc[i] - (cs_pc[ib] if ib >= 0 else 0.0)
        mv2 = float(int((s / c / 1e9) * 1e9)) if (c >= 10800.0 and c > 0) else 0.0
        out[q] = (pvraw[i], mv2)
    return out


def process_lid(lid):
    sym = SYMBOLS[lid]
    outdir = f"{FMT}/lid{lid}"; os.makedirs(outdir, exist_ok=True)
    ob = []; tr = []; bn = []; stored = {}
    for line in open(f"{DEMUX}/lid{lid}.jsonl", "rb"):
        d = json.loads(line); sr = d["source_raw"]
        ob += sr["ob"]; tr += sr["tr"]; bn += sr["bn"]; stored[d["ts_us"]] = d["factors"]
    ob_sorted = sorted(ob, key=lambda e: (e[0], e[13]))
    bn_sorted = sorted(bn, key=lambda e: (e[0], e[3]))
    TRSORT = os.environ.get("TRSORT", "ts")   # ts=按ts重排(原口径) / arr=保持到达序
    # OKX 表: OB 事件(price=0) 在前 + 成交(price>0) 在后 —— 与原 rows 拼接序一致。
    # ob=[ts,bid_px0,ask_px0,bid_sz0..4,ask_sz0..4,uid]; tr=[ts,side,px_scaled,amount,exch_ns,trade_id]
    n_ob, n_tr = len(ob_sorted), len(tr); n = n_ob + n_tr
    cols = build_cols(max(n, 1))              # n=0 → 一行全 0(同原 blank() 兜底)
    if n_ob:
        cols["ts"][:n_ob]  = np.fromiter((e[0] for e in ob_sorted), np.int64, n_ob)
        cols["bp0"][:n_ob] = np.fromiter((e[1] for e in ob_sorted), np.int64, n_ob)
        cols["ap0"][:n_ob] = np.fromiter((e[2] for e in ob_sorted), np.int64, n_ob)
        for i in range(5):
            cols[f"ba{i}"][:n_ob] = np.fromiter((e[3 + i] for e in ob_sorted), np.float64, n_ob)
            cols[f"aa{i}"][:n_ob] = np.fromiter((e[8 + i] for e in ob_sorted), np.float64, n_ob)
    if n_tr:
        s = slice(n_ob, n)
        cols["ts"][s]       = np.fromiter((e[0] for e in tr), np.int64, n_tr)
        cols["side"][s]     = np.fromiter((e[1] for e in tr), np.int64, n_tr)
        cols["price"][s]    = np.fromiter((e[2] for e in tr), np.int64, n_tr)
        cols["amount"][s]   = np.fromiter((e[3] for e in tr), np.float64, n_tr)
        cols["trade_id"][s] = np.fromiter((e[5] for e in tr), np.int64, n_tr)
    if TRSORT == "ts" and n:
        order = np.argsort(cols["ts"], kind="stable")   # 稳定: 同 ts 保持 OB 在前(同原 rows.sort(key=ts))
        for c in COLS: cols[c] = cols[c][order]         # 逐列换, 临时只多一列(不是整表翻倍)
    write_cols(cols, "okx_swap", sym, outdir)
    # BN 表: 只有 ts/bp0/ap0(同原 ob_row(ts,bpx,apx,[],[]))
    nb = len(bn_sorted)
    cols = build_cols(max(nb, 1))
    if nb:
        cols["ts"][:]  = np.fromiter((e[0] for e in bn_sorted), np.int64, nb)
        cols["bp0"][:] = np.fromiter((e[1] for e in bn_sorted), np.int64, nb)
        cols["ap0"][:] = np.fromiter((e[2] for e in bn_sorted), np.int64, nb)
    write_cols(cols, "binance_swap", sym, outdir)

    okx = sorted(glob.glob(f"{outdir}/okx_swap_{sym}_*.parquet"))
    bnp = sorted(glob.glob(f"{outdir}/binance_swap_{sym}_*.parquet"))
    first_ts = min(stored) if stored else None
    v2 = recompute_v2(ob_sorted, bn_sorted, sorted(stored))
    agg = {k: 0.0 for k, _ in COLMAP}; nd = {k: 0 for k, _ in COLMAP}; exc = {k: 0 for k, _ in COLMAP}
    aggv = {k: 0.0 for k in V2COLS}; ndv = {k: 0 for k in V2COLS}; excv = {k: 0 for k in V2COLS}; tot = 0
    for D in sorted(set(dateof(t) for t in stored)):
        try: ref = B.compute_day(okx, bnp, D)
        except Exception: continue
        if ref is None or ref.empty: continue
        rmap = {int(r.ts_us): r for r in ref.itertuples()}
        for t, fac in stored.items():
            if t == first_ts or dateof(t) != D or t not in rmap: continue
            if t < DFROM: continue                 # 暖机区不计 diff(仍进 compute_day/recompute 供暖机)
            tot += 1; rr = rmap[t]
            for sk, rk in COLMAP:
                if seam_hit(sk, t): exc[sk] += 1; continue
                dd = neq(fac[sk], getattr(rr, rk))
                if dd > agg[sk]: agg[sk] = dd
                if dd > 1e-9:
                    nd[sk] += 1
                    if sk in DBG:   # DBG_DIFF=f9,... → 打出该列每个有差秒(定位用)
                        print(f"  DIFF {sk} {sym} ts={t} 引擎={fac[sk]!r} 离线={getattr(rr, rk)!r} |d|={dd:.4e}", flush=True)
            rv2 = v2.get(t, (float("nan"), 0.0))
            for idx, k in enumerate(V2COLS):
                if seam_hit(k, t): excv[k] += 1; continue
                dd = neq(fac[k], rv2[idx])
                if dd > aggv[k]: aggv[k] = dd
                if dd > 1e-9:
                    ndv[k] += 1
                    if k in DBG:   # V2COLS 分支的有差秒定位(mean_v2/pdiff_v2)
                        print(f"  DIFF {k} {sym} ts={t} 引擎={fac[k]!r} 离线={rv2[idx]!r} |d|={dd:.4e}", flush=True)
    shutil.rmtree(outdir, ignore_errors=True)
    return (sym, agg, nd, aggv, ndv, tot, exc, excv)


def main():
    shutil.rmtree(WORK, ignore_errors=True); os.makedirs(DEMUX); os.makedirs(FMT)
    handles = {}; n = 0
    for line in open_dump(F):
        if line[:1] != b"{": continue
        try: d = json.loads(line)
        except Exception: continue
        t = d["ts_us"]
        if t < CLO or t >= CHI: continue
        lid = d["lid"]; h = handles.get(lid)
        if h is None: h = handles[lid] = open(f"{DEMUX}/lid{lid}.jsonl", "wb")
        h.write(line); n += 1
    for h in handles.values(): h.close()
    lids = sorted(handles)
    print(f"demux 完: {n} 行 -> {len(lids)} 币", flush=True)

    AGG = {k: 0.0 for k, _ in COLMAP}; ND = {k: 0 for k, _ in COLMAP}; EXC = {k: 0 for k, _ in COLMAP}
    AGGV = {k: 0.0 for k in V2COLS}; NDV = {k: 0 for k in V2COLS}; EXCV = {k: 0 for k in V2COLS}; TOT = 0
    for lid in lids:
        sym, agg, nd, aggv, ndv, tot, exc, excv = process_lid(lid)
        TOT += tot
        for sk, _ in COLMAP: AGG[sk] = max(AGG[sk], agg[sk]); ND[sk] += nd[sk]; EXC[sk] += exc[sk]
        for k in V2COLS: AGGV[k] = max(AGGV[k], aggv[k]); NDV[k] += ndv[k]; EXCV[k] += excv[k]
        print(f"  {sym} 完({tot} 秒, 累计 {TOT})", flush=True)

    print(f"\n===== 对比秒数={TOT}; 接缝区={'启用 '+os.environ.get('SEAM','') if SEAMS else '未剔除'} =====")
    print("列        max|diff|      有差秒   (剔除接缝秒)")
    for sk, _ in COLMAP: print(f"{sk:9s} {AGG[sk]:.3e}   {ND[sk]:<8d} ({EXC[sk]})")
    for k in V2COLS: print(f"{k:9s} {AGGV[k]:.3e}   {NDV[k]:<8d} ({EXCV[k]})")


if __name__ == "__main__":
    main()
