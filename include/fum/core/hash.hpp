// SHA-256: artifact digests, provenance ledger chaining, journal hash chain.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "fum/core/result.hpp"

namespace fum::hash {

inline constexpr std::size_t kSha256Bytes = 32;
using Sha256Digest = std::array<std::uint8_t, kSha256Bytes>;

[[nodiscard]] Sha256Digest sha256(std::string_view data) noexcept;
[[nodiscard]] Sha256Digest sha256(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] std::string to_hex(const Sha256Digest& digest);
[[nodiscard]] Result<Sha256Digest> from_hex(std::string_view hex);
[[nodiscard]] std::string sha256_hex(std::string_view data);
[[nodiscard]] bool constant_time_equal(std::string_view a, std::string_view b) noexcept;

// Streaming variant: bounded memory for large artifacts.
class Sha256Stream {
 public:
  Sha256Stream() noexcept;
  void update(std::string_view data) noexcept;
  [[nodiscard]] Sha256Digest finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;
  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finished_ = false;
};

}  // namespace fum::hash
