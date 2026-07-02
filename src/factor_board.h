#pragma once
// factor_board.h — tickfeat_live 输出段:per-LID 一个 seqlock 槽,存该 symbol 最新一秒的
// f0..f9 + mid + 数据时戳 + valid_mask(哪些因子暖机已满)。复用 gconf 的 SegHeader/seqlock/
// schema_fnv 框架原语(通用 SHM 框架,非段契约);FactorSlot/FactorBoard 本身是 tickfeat-local 契约。
//
// 下游按 tflive::FactorSlot 布局消费(按 LID 索引)。POD:只有字段,写读靠自由的 write/read。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/seqlock.h>
#include <gconf/symbol/symbol_idx.h>

namespace tflive {

inline constexpr int kNumFactors = 10;  // f0..f9

// 单 symbol 因子槽:一 cache line 对齐,seqlock 保护(偶=可读,奇=写入中)。
struct alignas(64) FactorSlot {
  std::atomic<std::uint64_t> seq{0};
  std::int64_t  ts_ns{0};        // 该行 1s 桶起点(数据 exch 时戳,ns)
  std::uint16_t symbol_lid{0};
  std::uint16_t valid_mask{0};   // bit i = fi 暖机已满可信(f0..f9→bit0..9;mid→bit10)
  std::uint32_t _rsvd{0};
  double        f[kNumFactors]{};
  double        mid{0.0};
};
static_assert(std::is_trivially_copyable_v<FactorSlot>, "FactorSlot 必须 POD(SHM 契约)");
static_assert(alignof(FactorSlot) == 64, "FactorSlot 一 cache line 对齐");
static_assert(offsetof(FactorSlot, seq) == 0, "seq 必须领头(seqlock)");

// 生产者:seqlock 包住字段写入(偶 s → 奇 s+1 → payload → 偶 s+2)。
inline void factor_write(FactorSlot& slot, std::int64_t ts_ns, std::uint16_t lid,
                         std::uint16_t valid_mask, const double* f10, double mid) noexcept {
  namespace seqlock = gconf::shm::v2::seqlock;
  const std::uint64_t s = slot.seq.load(std::memory_order_relaxed);
  seqlock::begin(slot.seq, s + 1);
  slot.ts_ns = ts_ns;
  slot.symbol_lid = lid;
  slot.valid_mask = valid_mask;
  for (int i = 0; i < kNumFactors; ++i) slot.f[i] = f10[i];
  slot.mid = mid;
  seqlock::end(slot.seq, s + 2);
}

// 消费者:返回 false = 读到撕裂,需重试。
[[nodiscard]] inline bool factor_read(const FactorSlot& slot, FactorSlot& out) noexcept {
  namespace seqlock = gconf::shm::v2::seqlock;
  const std::uint64_t s1 = seqlock::snapshot(slot.seq);
  if (s1 & 1) return false;
  out.ts_ns = slot.ts_ns;
  out.symbol_lid = slot.symbol_lid;
  out.valid_mask = slot.valid_mask;
  for (int i = 0; i < kNumFactors; ++i) out.f[i] = slot.f[i];
  out.mid = slot.mid;
  return seqlock::verify(slot.seq, s1);
}

inline constexpr std::uint64_t kFactorSchemaHash =
    gconf::shm::v2::schema_fnv("FactorSlot:seq8,ts8,lid2,vmask2,rsvd4,f10x8,mid8");

// 因子板:段头 + per-LID 槽。capacity = N_SYMS。
struct FactorBoard {
  gconf::shm::v2::SegHeader hdr;
  alignas(64) FactorSlot slot[gconf::sym::N_SYMS];
};
static_assert(std::is_standard_layout_v<FactorBoard>, "FactorBoard 必须 standard-layout(SHM 契约)");

}  // namespace tflive
