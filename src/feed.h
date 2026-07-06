#pragma once
// feed.h — 轮询三源一拍 → 去重换算成事件 → 全局 ts 序喂 per-LID 引擎。
//
// 轮询 latest-wins 段是生产消费模型(host 照做,不追离线全序);跨源用 (ts,prio) 稳序喂,
// OKX 优先——匹配离线 replay_feed 归并,尽量贴近但不保证跨所 bit-exact(f8/f9 有 ~1e-3bps 残差)。

#include <algorithm>
#include <cstdint>
#include <vector>

#include "streaming_engine.hpp"   // tick_feat::live::StreamingFeatureEngine
#include "event.hpp"

#include "convert.h"
#include "shm_in.h"

namespace tflive {

namespace tf = tick_feat::live;
using EngineSet = std::vector<tf::StreamingFeatureEngine>;

inline EngineSet make_engines() { return EngineSet(gconf::sym::N_SYMS); }

// 一条待喂事件(POD)。kind:0=ob 1=trade 2=bn;prio:0=OKX 1=BN(同 ts OKX 先)。
struct Pending {
  std::int64_t   ts;
  int            prio;
  int            kind;
  tf::ObEvent    ob;
  tf::TradeEvent tr;
  tf::BnMidEvent bn;
};

// 轮询/喂状态(POD)。batch 复用缓冲,不 per-tick 分配。
struct FeedState {
  std::vector<std::uint64_t>          okx_uid, bn_uid;   // update_id 去重
  std::vector<std::int64_t>           last_fed_ts;        // 单调守卫
  std::vector<std::vector<Pending>>   batch;              // per-LID,每拍清空复用
  std::uint64_t                       trade_tail = 0;
  std::uint64_t                       lapped = 0;
};

inline FeedState make_feed_state() {
  const int n = gconf::sym::N_SYMS;
  FeedState st;
  st.okx_uid.assign(n, 0);
  st.bn_uid.assign(n, 0);
  st.last_fed_ts.assign(n, INT64_MIN);
  st.batch.assign(n, {});
  return st;
}

// —— 收集(各源一件事)——

// BN 多档 book 轮询去重 → BnMidEvent(取 L0)。
// BN bookTicker 轮询去重 → BnMidEvent(单档 BBO 的 mid)。
// 不过滤(极端单边 bid/ask=0):与离线口径一致,引擎 bn_m>0 守卫使该秒基差作废(pdiff=0)。
// update_id!=bn_uid 已挡住未写过的槽(uid=0)。
inline void collect_bn(const Inputs& in, FeedState& st) {
  for (int lid = 0; lid < gconf::sym::N_SYMS; ++lid) {
    v2::BookTickBoardSlot s;
    if (in.bn_bt->slot[lid].read(s) && s.update_id != st.bn_uid[lid]) {
      st.bn_uid[lid] = s.update_id;
      const std::int64_t ts = ns_to_us(s.exch_ns);
      st.batch[lid].push_back(Pending{ts, 1, 2, {}, {},
          tf::BnMidEvent{ts, px_1e8(s.bid_px, s.price_scale), px_1e8(s.ask_px, s.price_scale), s.update_id}});
    }
  }
}

// OKX 成交无损 drain → TradeEvent(按 GID==LID 路由)。
inline void collect_trades(const Inputs& in, FeedState& st) {
  st.lapped += in.okx_tr->drain(st.trade_tail, [&](const v2::TradePayload& p) {
    const int lid = p.global_symbol_id;
    if (lid < 0 || lid >= gconf::sym::N_SYMS) return;
    const std::int64_t ts = ns_to_us(p.exch_ns);
    st.batch[lid].push_back(Pending{ts, 0, 1, {},
        tf::TradeEvent{ts, p.side, px_1e8(p.px, p.price_scale), amount_real(p.qty, p.qty_scale), p.exch_ns}, {}});
  });
}

// 从多档槽造 ObEvent(前 5 档)。
// OB 量喂【原始币量】(amount_real),不 ×1e8:离线 formatted 的 size 列就是原始小数(如 33.7),
// 引擎 imb 里对 size 统一 /PX_SCALE(与 python 参照 line72 同口径)。若这里 ×1e8,size 尺度比离线大 1e8,
// imb=(bid-ask)/(bid+ask) 虽尺度无关但浮点舍入差 1 ULP → f0/f1 残差 ~3e-16。喂原始量即与离线逐位一致。
inline Pending make_ob_pending(const v2::DepthBoardSlot& s) {
  const std::int64_t ts = ns_to_us(s.exch_ns);
  tf::ObEvent ev{};
  ev.ts = ts;
  ev.bid_px0 = px_1e8(s.bid_px[0], s.price_scale);
  ev.ask_px0 = px_1e8(s.ask_px[0], s.price_scale);
  for (int l = 0; l < 5; ++l) {
    ev.bid_sz[l] = amount_real(s.bid_qty[l], s.qty_scale);
    ev.ask_sz[l] = amount_real(s.ask_qty[l], s.qty_scale);
  }
  ev.update_id = s.update_id;   // 源血缘键(透传)
  return Pending{ts, 0, 0, ev, {}, {}};
}

// OKX 多档 book 轮询去重 → ObEvent。
inline void collect_okx_ob(const Inputs& in, FeedState& st) {
  for (int lid = 0; lid < gconf::sym::N_SYMS; ++lid) {
    v2::DepthBoardSlot s;
    if (in.okx_ob->slot[lid].read(s) && s.update_id != st.okx_uid[lid] && s.depth >= 1 && s.bid_px[0] && s.ask_px[0]) {
      st.okx_uid[lid] = s.update_id;
      st.batch[lid].push_back(make_ob_pending(s));
    }
  }
}

// 轮询三源一拍,填 st.batch;返回本拍事件总数。
inline std::size_t collect_tick(const Inputs& in, FeedState& st) {
  for (auto& v : st.batch) v.clear();
  collect_bn(in, st);
  collect_trades(in, st);
  collect_okx_ob(in, st);
  std::size_t total = 0;
  for (auto& v : st.batch) total += v.size();
  return total;
}

// —— 喂(全局 ts 序,单调守卫)——

inline void feed_one(tf::StreamingFeatureEngine& e, Pending& p, std::int64_t& last_ts) {
  if (p.ts < last_ts) p.ts = last_ts;  // 防跨源交错致 ts 倒退把 watermark 拉回
  last_ts = p.ts;
  if (p.kind == 0)      { p.ob.ts = p.ts; e.feed_okx_ob(p.ob); }
  else if (p.kind == 1) { p.tr.ts = p.ts; e.feed_okx_trade(p.tr); }
  else                  { p.bn.ts = p.ts; e.feed_bn_mid(p.bn); }
}

// 每 LID 按 (ts, prio) 稳序喂;单 LID 内确保非降序,OKX 与 BN 同 ts 时 OKX 先。
inline void feed_in_order(EngineSet& eng, FeedState& st) {
  for (int lid = 0; lid < gconf::sym::N_SYMS; ++lid) {
    auto& v = st.batch[lid];
    if (v.empty()) continue;
    std::stable_sort(v.begin(), v.end(), [](const Pending& a, const Pending& b) {
      return a.ts != b.ts ? a.ts < b.ts : a.prio < b.prio;
    });
    for (auto& p : v) feed_one(eng[lid], p, st.last_fed_ts[lid]);
  }
}

}  // namespace tflive
