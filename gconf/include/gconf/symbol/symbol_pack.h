#pragma once
#include <cstdint>
#include <string_view>

namespace gconf::sym {

// 编译期 Pack8: symbol name 低 8 字节按 ASCII 小端打包。
consteval std::uint64_t make_pack8(std::string_view s) noexcept {
  std::uint64_t v = 0;
  const std::size_t L = s.size();
  const std::size_t lim = (L < 8) ? L : 8;
  for (std::size_t i = 0; i < lim; ++i)
    v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(s[i])) << (8 * i);
  return v;
}

} // namespace gconf::sym
