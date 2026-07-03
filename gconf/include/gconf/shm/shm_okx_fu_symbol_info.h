#pragma once

#include <gconf/shm/shm_symbol_info_table.h>
#include <gconf/symbol/symbol_idx.h>

#include <cstdint>
#include <type_traits>

namespace gconf::shm::okx::fu::symbolInfo {

// ERR=0 哨兵：OKX 没有对应 BN 的 PERCENT_PRICE.multiplierUp / MIN_NOTIONAL.notional 概念,
// 保留 enum 仅为二进制布局对齐, REST SDK 永远写 ERR。
enum class UpType : std::uint8_t { ERR = 0, UP102, UP105, UP110, UP115, UP120, UP125, UP130, UP140 };

enum class MinNotional : std::uint8_t { ERR = 0, MIN_QTY_5, MIN_QTY_20, MIN_QTY_50 };

struct SymbolInfo {                            // 4B 慢变静态信息(每小时 REST 刷一次)
  std::uint8_t price_scale = 0;                // 1B OKX instrument.tickSz 小数位
  std::uint8_t qty_scale = 0;                  // 1B OKX instrument.lotSz 小数位
  UpType up_type = UpType::ERR;                // 1B 占位, OKX 不填(永远 ERR)
  MinNotional min_notional = MinNotional::ERR; // 1B 占位, OKX 不填(永远 ERR)
};
static_assert(sizeof(SymbolInfo) == 4, "SymbolInfo must stay 4B for L1-hot copy");
static_assert(std::is_trivially_copyable_v<SymbolInfo>, "SymbolInfo must be trivially copyable for std::atomic<SymbolInfo>");

// 跨进程共享表（模板实例化，每个交易所一个 alias）。
using OkxFuSymbolInfoTable = gconf::shm::SymbolInfoTable<SymbolInfo, gconf::sym::N_GIDS>;

} // namespace gconf::shm::okx::fu::symbolInfo
