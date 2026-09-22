// Structured, deterministic logging.
//
// Deadlock policy: a Logger holds its own mutex, so it must never be called
// while any runtime lock is held. The engine collects events locally and
// publishes them after releasing its state lock.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "fum/core/time.hpp"

namespace fum {

enum class LogLevel : std::uint8_t { trace = 0, debug = 1, info = 2, warn = 3, error = 4 };

[[nodiscard]] const char* log_level_name(LogLevel level) noexcept;

struct LogField {
  std::string key;
  std::string value;
};

struct LogRecord {
  LogLevel level = LogLevel::info;
  std::string message;
  std::vector<LogField> fields;
  Timestamp at;

  [[nodiscard]] std::string render() const;
};

class LogSink {
 public:
  LogSink() = default;
  LogSink(const LogSink&) = delete;
  LogSink& operator=(const LogSink&) = delete;
  virtual ~LogSink();
  virtual void write(const LogRecord& record) = 0;
};

class StderrLogSink final : public LogSink {
 public:
  void write(const LogRecord& record) override;
};

// Captures records in memory for assertions.
class MemoryLogSink final : public LogSink {
 public:
  void write(const LogRecord& record) override;
  [[nodiscard]] std::vector<LogRecord> snapshot() const;
  [[nodiscard]] std::size_t count_of(LogLevel level) const;

 private:
  mutable std::mutex mutex_;
  std::vector<LogRecord> records_;
};

class Logger {
 public:
  explicit Logger(std::shared_ptr<LogSink> sink = nullptr, LogLevel level = LogLevel::info);

  void set_level(LogLevel level) noexcept { level_ = level; }
  [[nodiscard]] LogLevel level() const noexcept { return level_; }
  void set_sink(std::shared_ptr<LogSink> sink);

  void log(LogLevel level, std::string message, std::vector<LogField> fields = {});

  void trace(std::string message, std::vector<LogField> fields = {});
  void debug(std::string message, std::vector<LogField> fields = {});
  void info(std::string message, std::vector<LogField> fields = {});
  void warn(std::string message, std::vector<LogField> fields = {});
  void error(std::string message, std::vector<LogField> fields = {});

 private:
  std::shared_ptr<LogSink> sink_;
  LogLevel level_ = LogLevel::info;
};

[[nodiscard]] LogField field(std::string key, std::string value);
[[nodiscard]] LogField field(std::string key, std::uint64_t value);
[[nodiscard]] LogField field(std::string key, std::int64_t value);
[[nodiscard]] LogField field(std::string key, bool value);

}  // namespace fum
