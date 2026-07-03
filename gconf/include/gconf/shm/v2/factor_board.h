#pragma once
// factor_board.h — v2 因子输出段(tickfeat_live 的 f0-f9 + mid + pdiff 派生因子板)。
//
// 与 depth_board.h / booktick_board.h 同套框架(SegHeader / seqlock / schema_fnv):per-LID 一个
// seqlock 槽,存该 symbol 最新一秒的因子。上游行情段(DepthBoard/TradeRing)是交易所原始行情;
// 本段是下游因子引擎(tickfeat_live)的产物,供策略/风控消费。capacity = N_SYMS,按 LID 索引。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/seqlock.h>
#include <gconf/symbol/symbol_idx.h>

namespace gconf::shm::v2 {

inline constexpr int kNumFactors = 10;  // f0..f9(imb1/imb5/spread/svol_ratio/vpin/trend60/trend300/rv300/pm_2h/pm_12h)

// 单 symbol 因子槽:一 cache line 对齐,seqlock 保护(偶=可读,奇=写入中)。
struct alignas(64) FactorSlot {
  std::atomic<std::uint64_t> seq{0};   // seqlock:偶=可读,奇=写入中
  std::int64_t  ts_ns{0};              // 该行 1s 桶起点(数据 exch 时戳,ns)
  std::uint16_t symbol_lid{0};         // LID
  std::uint16_t valid_mask{0};         // bit i = fi 暖机已满可信(f0..f9→bit0..9;mid→bit10;pdiff→bit11)
  std::uint32_t _rsvd{0};
  double        f[kNumFactors]{};      // f0..f9
  double        mid{0.0};              // 桶末 mid
  double        pdiff{0.0};            // 瞬时跨所基差(OKX mid−BN mid)/BN mid×1e4 bps;无 bn as-of=NaN(f8/f9 是其滚动均值)
};
static_assert(std::is_trivially_copyable_v<FactorSlot>, "FactorSlot 必须 POD(SHM 契约)");
static_assert(alignof(FactorSlot) == 64, "FactorSlot 一 cache line 对齐");
static_assert(offsetof(FactorSlot, seq) == 0, "seq 必须领头(seqlock)");
static_assert(offsetof(FactorSlot, ts_ns) == 8, "ts_ns @ +8");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "SHM seqlock atomic must be lock-free");

// 生产者:seqlock 包住字段写入(偶 s → 奇 s+1 → payload → 偶 s+2)。
inline void factor_write(FactorSlot& slot, std::int64_t ts_ns, std::uint16_t lid,
                         std::uint16_t valid_mask, const double* f10, double mid, double pdiff) noexcept {
  const std::uint64_t s = slot.seq.load(std::memory_order_relaxed);
  seqlock::begin(slot.seq, s + 1);
  slot.ts_ns = ts_ns;
  slot.symbol_lid = lid;
  slot.valid_mask = valid_mask;
  for (int i = 0; i < kNumFactors; ++i) slot.f[i] = f10[i];
  slot.mid = mid;
  slot.pdiff = pdiff;
  seqlock::end(slot.seq, s + 2);
}

// 消费者:返回 false = 读到撕裂,需重试。out.seq 不写(原子不拷贝)。
[[nodiscard]] inline bool factor_read(const FactorSlot& slot, FactorSlot& out) noexcept {
  const std::uint64_t s1 = seqlock::snapshot(slot.seq);
  if (s1 & 1) return false;
  out.ts_ns = slot.ts_ns;
  out.symbol_lid = slot.symbol_lid;
  out.valid_mask = slot.valid_mask;
  for (int i = 0; i < kNumFactors; ++i) out.f[i] = slot.f[i];
  out.mid = slot.mid;
  out.pdiff = slot.pdiff;
  return seqlock::verify(slot.seq, s1);
}

// schema_hash:本段字段布局的编译期 FNV;改布局即改串 → attach seg_check 告警。
inline constexpr std::uint64_t kFactorBoardSchemaHash =
    schema_fnv("FactorSlot:seq8,ts8,lid2,vmask2,rsvd4,f10x8,mid8,pdiff8");

// 因子板:段头 + per-LID 槽。capacity = N_SYMS(无 best_uid:单写者 tickfeat_live,不竞速)。
struct FactorBoard {
  SegHeader hdr;
  alignas(64) FactorSlot slot[gconf::sym::N_SYMS];
};
static_assert(std::is_standard_layout_v<FactorBoard>, "FactorBoard 必须 standard-layout(SHM 契约)");

}  // namespace gconf::shm::v2
