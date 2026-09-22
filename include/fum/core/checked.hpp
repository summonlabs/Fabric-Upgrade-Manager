// Checked arithmetic for externally derived sizes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "fum/core/result.hpp"

namespace fum::checked {

[[nodiscard]] inline Result<std::uint64_t> add_u64(std::uint64_t a, std::uint64_t b) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return make_error(ErrorCode::resource_exhausted, "integer overflow in addition",
                      "add_u64(" + std::to_string(a) + ", " + std::to_string(b) + ")");
  }
  return a + b;
}

[[nodiscard]] inline Result<std::uint64_t> mul_u64(std::uint64_t a, std::uint64_t b) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return make_error(ErrorCode::resource_exhausted, "integer overflow in multiplication",
                      "mul_u64(" + std::to_string(a) + ", " + std::to_string(b) + ")");
  }
  return a * b;
}

[[nodiscard]] inline Result<std::uint64_t> to_u64(std::size_t v) {
  return static_cast<std::uint64_t>(v);
}

// Narrowing helper that refuses values that do not round-trip.
[[nodiscard]] inline Result<std::size_t> to_size(std::uint64_t v) {
  if (v > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return make_error(ErrorCode::resource_exhausted, "value does not fit in size_t",
                      std::to_string(v));
  }
  return static_cast<std::size_t>(v);
}

// Guards against absurd externally supplied lengths before any allocation.
[[nodiscard]] inline Status require_bound(std::uint64_t value, std::uint64_t limit,
                                          std::string_view what) {
  if (value > limit) {
    return make_error(ErrorCode::resource_exhausted,
                      std::string(what) + " exceeds the configured bound",
                      std::to_string(value) + " > " + std::to_string(limit));
  }
  return ok_status();
}

}  // namespace fum::checked
