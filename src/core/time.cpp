#include "fum/core/time.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace fum {

Clock::~Clock() = default;

namespace {
constexpr std::int64_t kSecondsPerDay = 86400;
}  // namespace

const SystemClock& SystemClock::instance() {
  static const SystemClock clock;
  return clock;
}

Timestamp SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
  return Timestamp::from_unix_nanos(static_cast<std::int64_t>(nanos));
}

std::string Timestamp::to_iso8601() const {
  std::int64_t seconds = nanos_ / kNanosPerSecond;
  std::int64_t fraction = nanos_ % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    seconds -= 1;
  }
  const std::time_t as_time = static_cast<std::time_t>(seconds);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &as_time) != 0) {
    return "1970-01-01T00:00:00Z";
  }
#else
  if (gmtime_r(&as_time, &utc) == nullptr) {
    return "1970-01-01T00:00:00Z";
  }
#endif
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                utc.tm_sec, static_cast<long long>(fraction));
  return std::string(buffer);
}

Result<Timestamp> Timestamp::parse_iso8601(std::string_view text) {
  // Accepts YYYY-MM-DDTHH:MM:SS[.fraction]Z
  if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':' || text.back() != 'Z') {
    return make_error(ErrorCode::invalid_argument, "timestamp is not ISO-8601 UTC",
                      std::string(text));
  }
  auto digits = [&](std::size_t offset, std::size_t count) -> Result<int> {
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const char c = text[offset + i];
      if (c < '0' || c > '9') {
        return make_error(ErrorCode::invalid_argument, "timestamp contains a non-digit",
                          std::string(text));
      }
      value = value * 10 + (c - '0');
    }
    return value;
  };
  auto year = digits(0, 4);
  auto month = digits(5, 2);
  auto day = digits(8, 2);
  auto hour = digits(11, 2);
  auto minute = digits(14, 2);
  auto second = digits(17, 2);
  for (const auto* part : {&year, &month, &day, &hour, &minute, &second}) {
    if (!part->has_value()) {
      return part->error();
    }
  }
  if (month.value() < 1 || month.value() > 12 || day.value() < 1 || day.value() > 31 ||
      hour.value() > 23 || minute.value() > 59 || second.value() > 60) {
    return make_error(ErrorCode::invalid_argument, "timestamp component out of range",
                      std::string(text));
  }
  std::int64_t fraction_nanos = 0;
  if (text.size() > 20) {
    if (text[19] != '.' || text.size() < 22) {
      return make_error(ErrorCode::invalid_argument, "timestamp fraction is malformed",
                        std::string(text));
    }
    std::size_t index = 20;
    int scale = 100000000;
    while (index < text.size() - 1) {
      const char c = text[index];
      if (c < '0' || c > '9') {
        return make_error(ErrorCode::invalid_argument, "timestamp fraction contains a non-digit",
                          std::string(text));
      }
      if (scale > 0) {
        fraction_nanos += static_cast<std::int64_t>(c - '0') * scale;
        scale /= 10;
      }
      ++index;
    }
  }
  std::tm utc{};
  utc.tm_year = year.value() - 1900;
  utc.tm_mon = month.value() - 1;
  utc.tm_mday = day.value();
  utc.tm_hour = hour.value();
  utc.tm_min = minute.value();
  utc.tm_sec = second.value();
#if defined(_WIN32)
  const std::time_t seconds = _mkgmtime(&utc);
#else
  const std::time_t seconds = timegm(&utc);
#endif
  if (seconds == static_cast<std::time_t>(-1)) {
    return make_error(ErrorCode::invalid_argument, "timestamp could not be converted",
                      std::string(text));
  }
  const std::int64_t total = static_cast<std::int64_t>(seconds) * kNanosPerSecond + fraction_nanos;
  static_cast<void>(kSecondsPerDay);
  return Timestamp::from_unix_nanos(total);
}

}  // namespace fum
