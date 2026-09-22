// CRC-32C (Castagnoli) used for journal record integrity.
#pragma once

#include <cstdint>
#include <string_view>

namespace fum::crc {

[[nodiscard]] std::uint32_t crc32c(std::string_view data) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::uint32_t seed, std::string_view data) noexcept;

}  // namespace fum::crc
