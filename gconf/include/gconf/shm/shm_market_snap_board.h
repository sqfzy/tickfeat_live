#pragma once

#include <atomic>
#include <cstdint>

namespace gconf::shm {

template <typename Slot, int MaxSymbols> struct ShmMarketSnapBoard {
  // CAS 去重：每个 symbol 一个 atomic uid（交易所序列号）
  alignas(64) std::atomic<std::uint64_t> best_uid[MaxSymbols]{}; // 写者多，读者少。CAS 比较 update_id，避免浪费 SeqLock +跨核 invalidation

  // 赢家数据：每个 symbol 一个 slot（SeqLock 保护）
  alignas(64) Slot board[MaxSymbols]; // CAS 赢家才写。SeqLock 因为下游读者无法加锁，靠 seq 递增检测撕裂
};

} // namespace gconf::shm
