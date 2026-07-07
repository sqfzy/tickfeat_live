// event.hpp — 流式引擎与数据源解耦的事件契约
//
// 引擎只认这三种事件, 不认 RawTable/JSONL/WS/shm。replay 从 RawTable 行造事件,
// 未来 WsFeed/ShmFeed 产同样事件即可复用引擎。
//
// 价一律 ×1e8 的原始 int64、量一律真实值 double —— 与 RawTable / 离线 compute_day
// 的 `/PX_SCALE` 口径完全一致, 这样引擎内的 mid/imb/spread 走与离线同一条浮点路径。
#pragma once

#include <array>
#include <cstdint>

namespace tick_feat::live {

// OB 快照: 价 ×1e8 原始 int64; 前5档量真实值(引擎内 /PX_SCALE, 同离线 extract_ob_snapshots)
// update_id: 交易所盘口更新序号(惰性透传, 供源血缘; 不参与任何数值计算)
struct ObEvent {
    int64_t               ts;
    int64_t               bid_px0, ask_px0;
    std::array<double, 5> bid_sz, ask_sz;
    uint64_t              update_id{0};
};

// 成交: price ×1e8 原始; amount 真实币量; sign 由 side(0=主动买/1=主动卖)
// exch_ns: 交易所成交时戳(原始 ns, 惰性透传作源血缘键; ts 仍作分桶用)
struct TradeEvent {
    int64_t  ts;
    int64_t  side;
    int64_t  price_scaled;
    double   amount;
    int64_t  exch_ns{0};    // 交易所成交时戳(源血缘键之一)
    uint64_t trade_id{0};   // 交易所成交 ID(唯一单调, 逐笔溯源键)
};

// BN 行情(只取桶末 mid): 价 ×1e8 原始, 引擎按离线同表达式算 mid
// update_id: BookTick 盘口更新序号(惰性透传, 供源血缘)
struct BnMidEvent {
    int64_t  ts;
    int64_t  bid_px0, ask_px0;
    uint64_t update_id{0};
};

// OKX booktick(BBO, 1 档): mid/imb1/spread 的源(不再用 orderbook 的这三个)。
// 价 ×1e8 原始; L0 量原始币量(同 ObEvent, 引擎内 /PX_SCALE)。imb5 仍来自 ObEvent(5 档)。
struct OkxBookTickEvent {
    int64_t  ts;
    int64_t  bid_px0, ask_px0;
    double   bid_sz0, ask_sz0;
    uint64_t update_id{0};
};

} // namespace tick_feat::live
