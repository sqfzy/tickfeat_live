#pragma once

#include <gconf/shm/shm_symbol_info_table.h>
#include <gconf/symbol/symbol_idx.h>

#include <cstdint>
#include <type_traits>

// =============================================================================
// Bybit V5 Linear SymbolInfo（4B 慢变静态信息，bybit_syminfo_publisher
// 每整点拉一次 V5 /v5/market/instruments-info?category=linear 刷新发布）。
//
// 字段语义与 BN 同形（price_scale / qty_scale 等 1B 编码到 SCALED_VALUE）；
// bybit V5 用 priceFilter.tickSize / lotSizeFilter.qtyStep 字符串表达精度，
// publisher 解析后转 scale 整数填入。
// =============================================================================

namespace gconf::shm::bybit::lin::symbolInfo {

// minNotional 档位（暂用与 BN 同形枚举；bybit V5 实测主流 USDT pair 最小名义 5 USDT）
enum class MinNotional : std::uint8_t { ERR = 0, MIN_USDT_1, MIN_USDT_5, MIN_USDT_10 };

// 状态 enum（V5 status: "Trading" / "PreLaunch" / "Settling" / "Delivering" / "Closed"）
enum class TradingStatus : std::uint8_t { ERR = 0, TRADING, PRE_LAUNCH, SETTLING, DELIVERING, CLOSED };

struct SymbolInfo {                            // 4B 慢变静态信息
  std::uint8_t price_scale = 0;                // 1B tickSize → 小数位数
  std::uint8_t qty_scale = 0;                  // 1B qtyStep → 小数位数
  TradingStatus status = TradingStatus::ERR;   // 1B 交易状态
  MinNotional min_notional = MinNotional::ERR; // 1B 最小名义档位
};
static_assert(sizeof(SymbolInfo) == 4, "SymbolInfo must stay 4B for L1-hot copy");
static_assert(std::is_trivially_copyable_v<SymbolInfo>, "SymbolInfo must be trivially copyable for std::atomic<SymbolInfo>");

// 跨进程共享表（模板实例化，GidN 由全局 GID 空间提供）
using BybitLinSymbolInfoTable = gconf::shm::SymbolInfoTable<SymbolInfo, gconf::sym::N_GIDS>;

} // namespace gconf::shm::bybit::lin::symbolInfo
