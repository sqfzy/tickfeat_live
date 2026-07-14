#!/usr/bin/env python3
# funding_sub.py — 订阅 OKX/BN 资金费率, 写 FundingShm(默认 /shm_tickfeat_funding_v2), 供 tickfeat_live 引擎透传。
#
# 资金费率是交易所发布值(非从 tick 算): BN markPrice 流带 r(当前/预测费率); OKX funding-rate 频道带 fundingRate。
# 按 LID 写 64B seqlock 槽(布局与 src/funding_shm.h::FundingSlot 逐字一致)。okx/bn 各写各字段, 互不覆盖。
# 周期刷新: 每 REFRESH_S 重写最近值(更新 ts) → 引擎按 ts 老化判 valid; 订阅者死则超阈自动 stale(NaN)。
#
# 用法(云机直连): python3 funding_sub.py
#      (dev 需代理): FUNDING_PROXY=http://127.0.0.1:7890 python3 funding_sub.py
# 依赖: uv run --no-project --with aiohttp python3 funding_sub.py
import argparse, asyncio, json, mmap, os, struct, time
import aiohttp

# LID 序 == gconf symbol_idx.h kGidNames(append-only 契约; SOLUSDT=19)。
SYMS = ["AAVEUSDT", "ADAUSDT", "APTUSDT", "ARBUSDT", "AVAXUSDT", "AXSUSDT", "BCHUSDT", "BNBUSDT", "CRVUSDT",
        "DOGEUSDT", "DOTUSDT", "ENSUSDT", "HBARUSDT", "LTCUSDT", "OPUSDT", "ORDIUSDT", "PEOPLEUSDT", "PNUTUSDT",
        "SANDUSDT", "SOLUSDT", "SUIUSDT", "TIAUSDT", "TRUMPUSDT", "UNIUSDT", "WLDUSDT", "XLMUSDT", "XRPUSDT"]
N = len(SYMS)
BN2LID = {s: i for i, s in enumerate(SYMS)}                      # BN markPrice.s = "SOLUSDT"
OKX2LID = {s[:-4] + "-USDT-SWAP": i for i, s in enumerate(SYMS)}  # OKX instId = "SOL-USDT-SWAP"
SLOT = 64
REFRESH_S = 30

# FundingSlot 偏移(src/funding_shm.h): seq@0(Q) okx_ts@8(q) bn_ts@16(q) okx_rate@24(d) bn_rate@32(d) lid@40(H)
class Shm:
    def __init__(self, path):
        full = ("/dev/shm" + path) if path.startswith("/") else path
        size = N * SLOT
        exist = os.path.exists(full)
        fd = os.open(full, (os.O_CREAT | os.O_RDWR) if not exist else os.O_RDWR, 0o660)
        if not exist:
            os.ftruncate(fd, size)
        self.mm = mmap.mmap(fd, size)
        os.close(fd)
        self.last = {}   # (src,lid) -> rate, 供周期刷新

    def _wr(self, lid, ts_off, rate_off, rate):
        off = lid * SLOT
        seq = struct.unpack_from("<Q", self.mm, off)[0]
        struct.pack_into("<Q", self.mm, off, seq + 1)                # 奇: 写入中
        struct.pack_into("<q", self.mm, off + ts_off, time.time_ns())
        struct.pack_into("<d", self.mm, off + rate_off, rate)
        struct.pack_into("<H", self.mm, off + 40, lid)
        struct.pack_into("<Q", self.mm, off, seq + 2)                # 偶: 完成

    def wr_okx(self, lid, rate): self.last[("o", lid)] = rate; self._wr(lid, 8, 24, rate)
    def wr_bn(self, lid, rate):  self.last[("b", lid)] = rate; self._wr(lid, 16, 32, rate)

    def refresh(self):   # 重写最近值, 更新 ts(保鲜)
        for (src, lid), rate in list(self.last.items()):
            self._wr(lid, 8, 24, rate) if src == "o" else self._wr(lid, 16, 32, rate)


async def bn_task(shm, session, proxy):
    # BN futures WS(!markPrice@arr)从部分 IP 不吐流; 资金费率低频 → 用 premiumIndex REST 轮询(一次全 symbol)。
    # lastFundingRate = 当前/预测费率(与 markPrice.r 同义)。
    url = "https://fapi.binance.com/fapi/v1/premiumIndex"
    first = True
    while True:
        try:
            async with session.get(url, proxy=proxy, timeout=aiohttp.ClientTimeout(total=10)) as r:
                arr = await r.json()
            for e in arr:
                lid = BN2LID.get(e.get("symbol"))
                fr = e.get("lastFundingRate")
                if lid is not None and fr not in (None, ""):
                    shm.wr_bn(lid, float(fr))
            if first:
                print("[BN] premiumIndex REST polling", flush=True); first = False
        except Exception as ex:
            print("[BN] REST err:", ex, flush=True)
        await asyncio.sleep(5)   # 低频轮询(费率秒~分钟级变)


async def okx_task(shm, session, proxy):
    url = "wss://ws.okx.com:8443/ws/v5/public"
    args = [{"channel": "funding-rate", "instId": inst} for inst in OKX2LID]
    while True:
        try:
            async with session.ws_connect(url, proxy=proxy, heartbeat=25) as ws:
                await ws.send_json({"op": "subscribe", "args": args})
                print("[OKX] funding-rate subscribing", N, "insts", flush=True)
                async for msg in ws:
                    if msg.type != aiohttp.WSMsgType.TEXT:
                        continue
                    m = json.loads(msg.data)
                    if m.get("event"):
                        continue                                   # 订阅 ack/error
                    lid = OKX2LID.get((m.get("arg") or {}).get("instId"))
                    for d in (m.get("data") or []):
                        fr = d.get("fundingRate")
                        if lid is not None and fr not in (None, ""):
                            shm.wr_okx(lid, float(fr))
        except Exception as ex:
            print("[OKX] reconnect:", ex, flush=True)
            await asyncio.sleep(3)


async def refresh_task(shm):
    while True:
        await asyncio.sleep(REFRESH_S)
        shm.refresh()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shm", default="/shm_tickfeat_funding_v2")
    ap.add_argument("--proxy", default=os.environ.get("FUNDING_PROXY", ""))
    a = ap.parse_args()
    shm = Shm(a.shm)
    proxy = a.proxy or None
    print(f"funding_sub: shm={a.shm} proxy={proxy or '直连'} syms={N}", flush=True)
    async with aiohttp.ClientSession() as s:
        await asyncio.gather(bn_task(shm, s, proxy), okx_task(shm, s, proxy), refresh_task(shm))


if __name__ == "__main__":
    asyncio.run(main())
