#include "fum/core/crc.hpp"

#include <array>

namespace fum::crc {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> build_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0u ? (crc >> 1) ^ kPolynomial : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = build_table();

}  // namespace

std::uint32_t crc32c(std::uint32_t seed, std::string_view data) noexcept {
  std::uint32_t crc = ~seed;
  for (const char raw : data) {
    const auto byte = static_cast<std::uint8_t>(raw);
    crc = kTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

std::uint32_t crc32c(std::string_view data) noexcept { return crc32c(0u, data); }

}  // namespace fum::crc
