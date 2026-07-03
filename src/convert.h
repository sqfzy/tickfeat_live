#pragma once
// convert.h — 定点 mantissa(gconf 段的 uint32 ×10^scale)→ tick_feat 引擎单位。纯函数,无状态。
//
// 引擎口径(见 event.hpp):价一律 ×1e8 的 int64;OB 量一律【原始币量】(引擎 imb 内统一 /PX_SCALE,
// 与离线 formatted 的 size 原值口径逐位一致——若 ×1e8 会与离线尺度差 1e8、imb 浮点舍入偏 1 ULP);
// 成交 amount 一律真实币量(引擎 usd = amount × price/1e8);时间一律微秒(引擎 ts//1e6 = 秒桶)。

#include <cstdint>

namespace tflive {

inline constexpr std::int64_t kPow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

// 价 → ×1e8 的 int64(scale≤8;引擎收 int64,内部 /PX_SCALE)。
inline std::int64_t px_1e8(std::uint32_t mantissa, std::uint8_t scale) noexcept {
  return (scale <= 8) ? static_cast<std::int64_t>(mantissa) * kPow10[8 - scale] : 0;
}
// OB 量 / 成交量 → 真实币量(不 ×1e8;OB 量喂引擎也用它,见 event.hpp 口径说明)。
inline double amount_real(std::uint32_t mantissa, std::uint8_t scale) noexcept {
  return (scale <= 8) ? static_cast<double>(mantissa) / static_cast<double>(kPow10[scale]) : 0.0;
}
// 交易所纳秒 → 引擎微秒(引擎按 µs 分 1s 桶;ns 会误分成 ms 桶)。
inline std::int64_t ns_to_us(std::int64_t exch_ns) noexcept { return exch_ns / 1000; }

}  // namespace tflive
