#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace gconf::shm {

// 跨进程 SymbolInfo 共享表通用模板。
// Slot: per-exchange SymbolInfo（4B trivially copyable struct）;
// N:    该交易所/账户类型的 symbol 总数（编译期常量）。
// 整段放 POSIX shm,producer 创建,consumer 只读映射。
template <typename Slot, std::size_t N> struct alignas(64) SymbolInfoTable {
  using SlotType = Slot;                       // 暴露给 SymbolInfoMirror 等通用模板用
  static constexpr std::size_t kNumSlots = N;  // 同上

  std::atomic<std::uint32_t> epoch{0};        // 4B 0=未发布,每整轮 +1,consumer 用来判断是否需要重拉
  std::atomic<std::uint32_t> _pad32{0};       // 4B 对齐填充
  std::atomic<std::uint64_t> refreshed_ms{0}; // 8B producer 完整刷新一轮的 wall clock(ms)
  char _pad_to_line[64 - 16]{};               // 让 slots[0] 独占下一 cacheline
  std::atomic<Slot> slots[N]{};

  // Producer 单 slot 发布：构造完整 Slot 后一次性原子 store。
  void publish_slot(std::uint16_t gid, Slot info) noexcept { slots[gid].store(info, std::memory_order_release); }

  // Producer 整轮发布：推进 epoch + 时间戳，触发 consumer mirror 重拉。
  // release 保证所有 publish_slot 在 epoch 自增前对其它进程可见。
  void publish_round(std::uint64_t now_ms) noexcept {
    refreshed_ms.store(now_ms, std::memory_order_relaxed);
    epoch.fetch_add(1, std::memory_order_release);
  }

  // Producer 自读（同进程；跨进程消费者请走 SymbolInfoMirror）。
  [[nodiscard]] Slot load_local(std::uint16_t gid) const noexcept { return slots[gid].load(std::memory_order_acquire); }

  static_assert(std::atomic<Slot>::is_always_lock_free, "Slot must fit lock-free atomic (<=8B on x86_64)");
  static_assert(std::is_trivially_destructible_v<Slot>, "Slot must be trivially destructible for OwnedShmBoard");
};

} // namespace gconf::shm
