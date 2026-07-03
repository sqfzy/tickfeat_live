#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/seqlock.h>

namespace gconf::shm::v2 {

// MPMC 广播环 (Vyukov bounded + per-cell slot-encoded seqlock):
//   - 多生产者: 各 fetch_add(head) 抢唯一绝对槽, 在 entry[slot&MASK] 上 seqlock 写; seq 编码 (slot<<1)|writing。
//   - 多消费者 (广播): tail 在 SHM 外 (各消费者各持一份, 不背压), 据 seq 识别本圈数据并检测 lapped
//     (head-tail>Cap = 被绕圈, 丢了 head-tail-Cap 条 → 返回丢失计数, 调用方据此 resync)。
// Entry 约定: { std::atomic<std::uint64_t> seq; Payload p; }, seq @0 (seqlock 圈号), p @8 (纯 POD)。
template <class Entry, std::size_t Cap> struct BcastRing {
  static_assert((Cap & (Cap - 1)) == 0, "Cap must be a power of two");
  static constexpr std::size_t CAP = Cap;
  static constexpr std::size_t MASK = Cap - 1;
  using Payload = decltype(Entry::p);

  SegHeader hdr;
  alignas(64) std::atomic<std::uint64_t> head{0}; // producer fetch_add (独占行, 防 false sharing)
  alignas(64) Entry entries[Cap];

  // 生产者:fetch_add 抢绝对槽 + seqlock 写 payload (seq 编码 slot, 供消费者识本圈 / 检 lapped)。
  // 返回抢到的绝对 slot 序号 (单调递增), 调用方可作 req id / 日志用。
  std::uint64_t publish(const Payload &pl) noexcept {
    // 兜底: 段头未 stamp (magic=0) ⇒ creator 走了裸 open_zero_init 忘 stamp。debug 当场炸, release(NDEBUG) 零成本。
    assert(hdr.magic == kSegMagic && "BcastRing 未 stamp 段头: creator 须用 v2::create_segment() 建段");
    const std::uint64_t slot = head.fetch_add(1, std::memory_order_relaxed);
    Entry &e = entries[slot & MASK];
    seqlock::begin(e.seq, (slot << 1) | 1); // 奇 (slot 编码) + 前沿 fence
    e.p = pl;
    seqlock::end(e.seq, slot << 1); // 偶 (slot 编码) + 后沿 release
    return slot;
  }

  // 消费者:扫 [tail..head) 派发 fn(const Payload&)。返回【被绕圈丢失】的条数 (>0 ⇒ 该 resync)。
  // 未就绪槽 (写入中 / MPMC 乱序未补齐) → break 等下一拍, 不静默跳过 (不丢)。
  template <class Fn> std::uint64_t drain(std::uint64_t &tail, Fn &&fn) const noexcept {
    const std::uint64_t h = head.load(std::memory_order_acquire);
    std::uint64_t lapped = 0;
    if (h - tail > Cap) {        // 被绕圈: tail 跳到最近可读
      lapped = (h - tail) - Cap; // 丢了这么多条
      tail = h - Cap;            // tail 跳到最近仍可读处
    }
    for (; tail < h; ++tail) {
      const Entry &e = entries[tail & MASK];
      const std::uint64_t s1 = seqlock::snapshot(e.seq);
      if (s1 != (tail << 1))
        break;          // 本槽尚未写好 (写入中 / 还没轮到) → 等
      Payload pl = e.p; // 拷贝 payload (不碰 atomic)
      if (!seqlock::verify(e.seq, s1))
        break; // 期间被写者改 → 重读
      fn(pl);
    }
    return lapped; // 上报丢失数 → 调用方据此 resync
  }
};

} // namespace gconf::shm::v2
