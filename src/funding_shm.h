#pragma once
// funding_shm.h — 内部资金费率输入段(订阅者写, 引擎读)。
//
// 资金费率是交易所【发布值】(每 8h 结算, 当前/预测费率秒~分钟级刷新), 非从 tick 算出。
// 订阅者(funding_sub.py, 连 OKX funding-rate + BN markPrice WS)按 LID 写本段最新费率;
// 引擎每轮 poll 读, 把最新费率透传进 FactorBoard 的 okx_funding/bn_funding(bit14/15)。
//
// 非 gconf 跨队契约(仅 tickfeat_live 内部两端), 故无 SegHeader/schema_check —— 两端共识 64B 槽布局即可。
// per-LID seqlock 槽: 写者 seq 偶→奇→写字段→偶; 读者撕裂即重试。okx/bn 两源独立更新(各写各字段)。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/symbol/symbol_idx.h>

namespace tflive {

struct alignas(64) FundingSlot {
  std::atomic<std::uint64_t> seq{0};   // seqlock: 偶=可读, 奇=写入中
  std::int64_t  okx_ts_ns{0};          // OKX 费率最近更新时戳(ns); 0=从未
  std::int64_t  bn_ts_ns{0};           // BN 费率最近更新时戳(ns); 0=从未
  double        okx_rate{0.0};         // OKX 当前/预测资金费率(原始小数, 如 0.0001=0.01%)
  double        bn_rate{0.0};          // BN 当前/预测资金费率(原始小数)
  std::uint16_t symbol_lid{0};         // LID(冗余校验)
  std::uint8_t  _rsvd[22]{};

  // 读: 撕裂(写入中或期间被改)返 false, 调用方重试。out.seq 不拷。
  [[nodiscard]] bool read(FundingSlot& out) const noexcept {
    const std::uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1) return false;
    out.okx_ts_ns = okx_ts_ns; out.bn_ts_ns = bn_ts_ns;
    out.okx_rate = okx_rate;   out.bn_rate = bn_rate;
    out.symbol_lid = symbol_lid;
    std::atomic_thread_fence(std::memory_order_acquire);
    return seq.load(std::memory_order_relaxed) == s1;   // 期间无写者 ⇔ 一致
  }
};
static_assert(sizeof(FundingSlot) == 64, "FundingSlot 必须 64B 一 cache line(SHM 契约, Python 端按此布局写)");
static_assert(alignof(FundingSlot) == 64, "FundingSlot 一 cache line 对齐");
static_assert(offsetof(FundingSlot, seq) == 0, "seq 领头(seqlock)");
static_assert(offsetof(FundingSlot, okx_ts_ns) == 8 && offsetof(FundingSlot, bn_ts_ns) == 16
              && offsetof(FundingSlot, okx_rate) == 24 && offsetof(FundingSlot, bn_rate) == 32
              && offsetof(FundingSlot, symbol_lid) == 40,
              "FundingSlot 字段偏移是 SHM 契约(Python 写者按此) —— 锁定");
static_assert(std::is_trivially_copyable_v<FundingSlot>, "FundingSlot 必须 POD");

// 段: 纯 per-LID 槽数组(无段头), 容量 = N_SYMS。名: /shm_tickfeat_funding_v2。
struct FundingShm {
  FundingSlot slot[gconf::sym::N_SYMS];
};
static_assert(std::is_standard_layout_v<FundingShm>, "FundingShm 必须 standard-layout(SHM 契约)");

// funding 新鲜度阈值: 超此(ns)未更新视为 stale, 引擎不置 valid。费率低频(BN ~1s, OKX 数分钟), 给宽松 10min。
inline constexpr std::int64_t kFundingStaleNs = 600LL * 1'000'000'000LL;

}  // namespace tflive
