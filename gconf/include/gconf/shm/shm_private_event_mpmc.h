#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gconf::shm {

// 私有事件 ring 默认容量: 私有 TPS 通常 < 200, 峰值 < 1000; 1024 留足 backlog.
inline constexpr std::size_t PRIVATE_EVENT_RING_CAP = 1024;

// MPMC ring board for 私有事件 stream (广播/路由型):
//   - Producer: N 路 dispatcher 共享, dedup 通过后 fetch_add head 抢一格写入
//   - Consumer: M 个 (日志写盘 / 策略观察 / latency 记录 ...), 各自看到所有 entry
//   - 每个 consumer 持本地 tail (栈/全局变量, 不放 SHM), load(head, acquire) 后扫

template <typename Entry, std::size_t Cap> struct PrivateEventRing {
  static_assert((Cap & (Cap - 1)) == 0, "Cap must be power of 2");
  static constexpr std::size_t MASK = Cap - 1;

  // producer 入口: fetch_add 抢 slot 编号, slot_id & MASK 为 entries 下标
  alignas(64) std::atomic<std::uint64_t> head{0};
  // 环形数组
  alignas(64) Entry entries[Cap];
};

} // namespace gconf::shm
