#pragma once

#include <cstdint>
#include <gconf/shm/v2/seg_header.h> // SegHeader / SegKind / SegError / seg_init / seg_check

// 每个 v2 段类型的"段头契约"单一事实源 SegSpec<Ring>:生产端 stamp、消费端 verify、建段 create_segment
// 三方全部从此取 kind/entry_size/capacity/schema_hash,不再各自手写一遍(手写两端易漂移,见 seg_header.h #82)。
//
// 用法:
//   - 每个 ring/segment 类型在其 alias 旁特化 SegSpec<T>(见 account_event_board.h / order_req_spmc.h)。
//   - 生产 creator: create_segment<Ring>(board, name, now_ns)  // open_zero_init + stamp 二合一, 不会忘 stamp
//   - 消费 attach : if (seg_verify<Ring>(ring->hdr) != SegError::Ok) return false;
//   - 没特化 SegSpec 的类型实例化以下任一模板 → 直接编译错(强制先补契约)。

namespace gconf::shm::v2 {

// 主模板不定义:未特化的段类型用到下面任一模板即编译错。
template <class Ring> struct SegSpec;

// 生产端:建段后 stamp 段头(open_zero_init 只 ftruncate 清零, 不写头)。
template <class Ring> inline void seg_stamp(Ring &r, std::uint64_t now_ns) noexcept {
  using S = SegSpec<Ring>;
  seg_init(r.hdr, S::kind, S::entry_size, S::capacity, S::schema_hash, now_ns);
}

// 消费端:attach 后校验段头(与本端编译期 SegSpec 逐字段比对)。
template <class Ring> [[nodiscard]] inline SegError seg_verify(const SegHeader &h) noexcept {
  using S = SegSpec<Ring>;
  return seg_check(h, S::kind, S::entry_size, S::capacity, S::schema_hash);
}

// 建段唯一入口:open_zero_init(清零) + seg_stamp(写头)二合一, 杜绝"建了忘 stamp"。
// Board 走鸭子类型(要求 .open_zero_init(name) 返回 bool + .get() 返回 Ring*), gconf 不耦合具体 posix_shm 实现。
template <class Ring, class Board> [[nodiscard]] inline bool create_segment(Board &board, const char *name, std::uint64_t now_ns) noexcept {
  if (!board.open_zero_init(name))
    return false;
  seg_stamp<Ring>(*board.get(), now_ns);
  return true;
}

} // namespace gconf::shm::v2
