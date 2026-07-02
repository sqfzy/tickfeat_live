#pragma once
// convert.h — 定点 mantissa(gconf 段的 uint32 ×10^scale)→ tick_feat 引擎单位。纯函数,无状态。
//
// 引擎口径(见 cpp/src/live/event.hpp):价一律 ×1e8 的 int64;OB 量一律 ×1e8(引擎内 /1e8);
// 成交 amount 一律真实币量(引擎 usd = amount × price/1e8);时间一律微秒(引擎 ts//1e6 = 秒桶)。

#include <cstdint>

namespace tflive {

inline constexpr std::int64_t kPow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

// 价 / OB 量 → ×1e8(scale≤8)。价用 int64,OB 量用 double(引擎收 double,内部 /1e8)。
inline std::int64_t px_1e8(std::uint32_t mantissa, std::uint8_t scale) noexcept {
  return (scale <= 8) ? static_cast<std::int64_t>(mantissa) * kPow10[8 - scale] : 0;
}
inline double size_1e8(std::uint32_t mantissa, std::uint8_t scale) noexcept {
  return static_cast<double>(px_1e8(mantissa, scale));
}
// 成交量 → 真实币量(不 ×1e8)。
inline double amount_real(std::uint32_t mantissa, std::uint8_t scale) noexcept {
  return (scale <= 8) ? static_cast<double>(mantissa) / static_cast<double>(kPow10[scale]) : 0.0;
}
// 交易所纳秒 → 引擎微秒(引擎按 µs 分 1s 桶;ns 会误分成 ms 桶)。
inline std::int64_t ns_to_us(std::int64_t exch_ns) noexcept { return exch_ns / 1000; }

}  // namespace tflive
