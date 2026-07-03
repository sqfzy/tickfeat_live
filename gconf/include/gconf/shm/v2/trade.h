#pragma once
// gconf/shm/v2/trade.h — v2 成交广播环(market taker trade stream)。
//
// 复用 mpmc_ring.h 的 BcastRing<Entry,Cap>(Vyukov per-cell seq + Disruptor):
// 生产者 fetch_add 抢绝对槽、seqlock 写,消费者 drain 检测 lapped。成交是「无损事件流」语义
// (每一笔都要,不能 latest-wins 覆盖),故走 BcastRing 而非 board。
//
// ⚠️ 字段是行情 taker 成交的最小集(exch_ns/px/qty/gid/side/scale);扩字段填 _rsvd,
//    消费者按 schema_hash 自校验防错位。
//
// 同步:TradeEntry = { atomic seq(seqlock) ; TradePayload p(纯 POD,可拷贝) },符合 BcastRing 的
// Entry 约定(seq@0 圈号 / p@8 payload)。读者拷贝 p,用 seqlock::verify 确认一致。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/shm/v2/mpmc_ring.h>   // BcastRing<Entry,Cap>
#include <gconf/shm/v2/seg_header.h>  // schema_fnv

namespace gconf::shm::v2 {

// 成交 payload(纯 POD,可拷贝;不含 atomic)。56B,使 TradeEntry 凑齐 64B(同 EventPayload 套路)。
struct TradePayload {
  std::int64_t  exch_ns{0};           // 交易所成交时间戳(全精度 ns)
  std::uint32_t px{0}, qty{0};        // 价/量 ×10^scale
  std::uint16_t global_symbol_id{0};  // GID(行情类随 board 用 global symbol id,非 local)
  std::uint8_t  side{0};              // 0 = taker buy(+1) / 1 = sell(-1)
  std::uint8_t  price_scale{0}, qty_scale{0};
  std::uint8_t  _rsvd[35]{};          // 同版本内加字段填此处,sizeof 不变
};
static_assert(sizeof(TradePayload) == 56, "TradePayload must be 56B (TradeEntry → 64B)");
static_assert(std::is_trivially_copyable_v<TradePayload>, "TradePayload must be POD");
// 字段偏移即 SHM 契约:两进程 padding 即便 sizeof 相等也可能偏移不同 → 逐字段错位静默错乱(#82)。
static_assert(offsetof(TradePayload, exch_ns) == 0 && offsetof(TradePayload, px) == 8 &&
              offsetof(TradePayload, qty) == 12 && offsetof(TradePayload, global_symbol_id) == 16 &&
              offsetof(TradePayload, side) == 18,
              "TradePayload field offsets are the SHM contract — lock them");

struct alignas(64) TradeEntry {
  std::atomic<std::uint64_t> seq{0};  // seqlock:(slot<<1)|writing;0 = 从未写
  TradePayload               p;
};
static_assert(sizeof(TradeEntry) == 64, "TradeEntry must be one cache line");
static_assert(alignof(TradeEntry) == 64, "TradeEntry must be cache-line aligned");
static_assert(offsetof(TradeEntry, seq) == 0 && offsetof(TradeEntry, p) == 8,
              "TradeEntry seq must lead (seqlock 圈号编码);payload @8 (#82)");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "SHM seq atomic must be lock-free — 跨进程同步前提(#90)");

inline constexpr std::uint64_t kTradeSchemaHash = schema_fnv(
    "TradeEntry:seq8|exch_ns8,px4,qty4,gid2,side1,pscale1,qscale1");

inline constexpr std::size_t TRADE_RING_CAP = 4096;  // 2 的幂;成交比 order event 密,留大些

using TradeRing = BcastRing<TradeEntry, TRADE_RING_CAP>;
static_assert(std::is_standard_layout_v<TradeRing>, "TradeRing must be standard-layout (SHM contract)");

}  // namespace gconf::shm::v2
