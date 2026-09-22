// Time is injected: production uses the system clock, tests use a manual clock.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "fum/core/result.hpp"

namespace fum {

inline constexpr std::int64_t kNanosPerMicrosecond = 1000;
inline constexpr std::int64_t kNanosPerMillisecond = 1000 * 1000;
inline constexpr std::int64_t kNanosPerSecond = 1000 * 1000 * 1000;

class [[nodiscard]] Duration {
 public:
  constexpr Duration() = default;
  [[nodiscard]] static constexpr Duration from_nanos(std::int64_t v) { return Duration(v); }
  [[nodiscard]] static constexpr Duration from_micros(std::int64_t v) {
    return Duration(v * kNanosPerMicrosecond);
  }
  [[nodiscard]] static constexpr Duration from_millis(std::int64_t v) {
    return Duration(v * kNanosPerMillisecond);
  }
  [[nodiscard]] static constexpr Duration from_seconds(std::int64_t v) {
    return Duration(v * kNanosPerSecond);
  }
  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr double seconds() const noexcept {
    return static_cast<double>(nanos_) / static_cast<double>(kNanosPerSecond);
  }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }

  friend constexpr bool operator==(Duration a, Duration b) { return a.nanos_ == b.nanos_; }
  friend constexpr bool operator!=(Duration a, Duration b) { return a.nanos_ != b.nanos_; }
  friend constexpr bool operator<(Duration a, Duration b) { return a.nanos_ < b.nanos_; }
  friend constexpr bool operator<=(Duration a, Duration b) { return a.nanos_ <= b.nanos_; }
  friend constexpr bool operator>(Duration a, Duration b) { return b < a; }
  friend constexpr bool operator>=(Duration a, Duration b) { return b <= a; }
  friend constexpr Duration operator+(Duration a, Duration b) {
    return Duration(a.nanos_ + b.nanos_);
  }
  friend constexpr Duration operator-(Duration a, Duration b) {
    return Duration(a.nanos_ - b.nanos_);
  }

 private:
  constexpr explicit Duration(std::int64_t nanos) : nanos_(nanos) {}
  std::int64_t nanos_ = 0;
};

// Absolute instant on the UNIX epoch, nanosecond resolution, UTC.
class [[nodiscard]] Timestamp {
 public:
  constexpr Timestamp() = default;
  [[nodiscard]] static constexpr Timestamp from_unix_nanos(std::int64_t v) {
    return Timestamp(v);
  }
  [[nodiscard]] static Result<Timestamp> parse_iso8601(std::string_view text);

  [[nodiscard]] constexpr std::int64_t unix_nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
  [[nodiscard]] std::string to_iso8601() const;

  [[nodiscard]] constexpr Timestamp plus(Duration d) const { return Timestamp(nanos_ + d.nanos()); }
  [[nodiscard]] constexpr Duration since(Timestamp earlier) const {
    return Duration::from_nanos(nanos_ - earlier.nanos_);
  }
  [[nodiscard]] constexpr bool after(Timestamp other) const { return nanos_ > other.nanos_; }
  [[nodiscard]] constexpr bool before(Timestamp other) const { return nanos_ < other.nanos_; }

  friend constexpr bool operator==(Timestamp a, Timestamp b) { return a.nanos_ == b.nanos_; }
  friend constexpr bool operator!=(Timestamp a, Timestamp b) { return a.nanos_ != b.nanos_; }
  friend constexpr bool operator<(Timestamp a, Timestamp b) { return a.nanos_ < b.nanos_; }
  friend constexpr bool operator<=(Timestamp a, Timestamp b) { return a.nanos_ <= b.nanos_; }
  friend constexpr bool operator>(Timestamp a, Timestamp b) { return b < a; }
  friend constexpr bool operator>=(Timestamp a, Timestamp b) { return b <= a; }

 private:
  constexpr explicit Timestamp(std::int64_t nanos) : nanos_(nanos) {}
  std::int64_t nanos_ = 0;
};

class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock();
  [[nodiscard]] virtual Timestamp now() const = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] static const SystemClock& instance();
  [[nodiscard]] Timestamp now() const override;
};

// Deterministic clock: time only moves when a test moves it.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(Timestamp start = Timestamp::from_unix_nanos(1700000000LL * kNanosPerSecond))
      : now_(start) {}
  [[nodiscard]] Timestamp now() const override { return now_; }
  void advance(Duration delta) { now_ = now_.plus(delta); }
  void set(Timestamp value) { now_ = value; }

 private:
  Timestamp now_;
};

}  // namespace fum
