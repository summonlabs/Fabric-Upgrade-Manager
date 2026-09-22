// Fabric Upgrade Manager - Summon Software Labs
// Error and result primitives used across the runtime.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fum {

// Stable, machine-readable failure classification. Every failure surfaced by
// the runtime carries one of these; nothing is reported as a bare string.
enum class ErrorCode : std::uint8_t {
  ok = 0,
  invalid_argument,
  not_found,
  already_exists,
  conflict,
  precondition_failed,
  fenced,              // stale generation/epoch/incarnation/attempt replay
  stale,               // evidence or preflight ticket older than its validity
  integrity_failure,   // digest/signature/chain mismatch
  corrupt,             // structure could not be trusted
  io_error,
  timeout,
  cancelled,
  unsupported,         // asked for something the adapter truthfully cannot do
  not_integrated,      // depends on an external Fabric system that is not wired
  policy_denied,
  incompatible,
  irreversible,        // plan contains an irreversible step that was not acknowledged
  resource_exhausted,
  internal,
  closed,
};

[[nodiscard]] const char* error_code_name(ErrorCode code) noexcept;

class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message, std::string detail = {});

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::ok; }

  // Deterministic rendering: "<code>: <message>" with " [<detail>]" appended.
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Error& a, const Error& b) {
    return a.code_ == b.code_ && a.message_ == b.message_ && a.detail_ == b.detail_;
  }

 private:
  ErrorCode code_ = ErrorCode::ok;
  std::string message_;
  std::string detail_;
};

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)
  Result(ErrorCode code, std::string message) : error_(code, std::move(message)) {}

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const Error& error() const noexcept { return error_; }

  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] const T& value() const& { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }

  [[nodiscard]] T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }

 private:
  std::optional<T> value_;
  Error error_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)
  Result(ErrorCode code, std::string message) : error_(code, std::move(message)) {}

  [[nodiscard]] bool has_value() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return error_.ok(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  Error error_;
};

using Status = Result<void>;

[[nodiscard]] inline Status ok_status() { return Status{}; }

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message, std::string detail = {}) {
  return Error(code, std::move(message), std::move(detail));
}

// Assignment form of error propagation. `dest` must be an already declared
// lvalue; the temporary Result stays alive until the end of the statement.
#define FUM_TRY(dest, expr)                         \
  do {                                              \
    auto fum_try_result = (expr);                   \
    if (!fum_try_result.has_value()) {              \
      return fum_try_result.error();                \
    }                                               \
    (dest) = std::move(fum_try_result).value();     \
  } while (false)

#define FUM_TRYV(expr)                              \
  do {                                              \
    auto fum_try_result = (expr);                   \
    if (!fum_try_result.has_value()) {              \
      return fum_try_result.error();                \
    }                                               \
  } while (false)

}  // namespace fum
