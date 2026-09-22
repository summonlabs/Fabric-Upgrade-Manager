#include "fum/core/log.hpp"

#include <cstdio>

namespace fum {

LogSink::~LogSink() = default;

const char* log_level_name(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::trace: return "trace";
    case LogLevel::debug: return "debug";
    case LogLevel::info: return "info";
    case LogLevel::warn: return "warn";
    case LogLevel::error: return "error";
  }
  return "unknown";
}

std::string LogRecord::render() const {
  std::string out;
  out.append(at.to_iso8601());
  out.push_back(' ');
  out.append(log_level_name(level));
  out.push_back(' ');
  out.append(message);
  for (const auto& f : fields) {
    out.push_back(' ');
    out.append(f.key);
    out.push_back('=');
    out.append(f.value);
  }
  return out;
}

void StderrLogSink::write(const LogRecord& record) {
  const std::string text = record.render();
  std::fprintf(stderr, "%s\n", text.c_str());
}

void MemoryLogSink::write(const LogRecord& record) {
  const std::lock_guard<std::mutex> lock(mutex_);
  records_.push_back(record);
}

std::vector<LogRecord> MemoryLogSink::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return records_;
}

std::size_t MemoryLogSink::count_of(LogLevel level) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::size_t count = 0;
  for (const auto& record : records_) {
    if (record.level == level) {
      ++count;
    }
  }
  return count;
}

Logger::Logger(std::shared_ptr<LogSink> sink, LogLevel level)
    : sink_(sink ? std::move(sink) : std::make_shared<StderrLogSink>()), level_(level) {}

void Logger::set_sink(std::shared_ptr<LogSink> sink) {
  sink_ = sink ? std::move(sink) : std::make_shared<StderrLogSink>();
}

void Logger::log(LogLevel level, std::string message, std::vector<LogField> fields) {
  if (static_cast<std::uint8_t>(level) < static_cast<std::uint8_t>(level_)) {
    return;
  }
  if (!sink_) {
    return;
  }
  LogRecord record;
  record.level = level;
  record.message = std::move(message);
  record.fields = std::move(fields);
  record.at = SystemClock::instance().now();
  sink_->write(record);
}

void Logger::trace(std::string message, std::vector<LogField> fields) {
  log(LogLevel::trace, std::move(message), std::move(fields));
}
void Logger::debug(std::string message, std::vector<LogField> fields) {
  log(LogLevel::debug, std::move(message), std::move(fields));
}
void Logger::info(std::string message, std::vector<LogField> fields) {
  log(LogLevel::info, std::move(message), std::move(fields));
}
void Logger::warn(std::string message, std::vector<LogField> fields) {
  log(LogLevel::warn, std::move(message), std::move(fields));
}
void Logger::error(std::string message, std::vector<LogField> fields) {
  log(LogLevel::error, std::move(message), std::move(fields));
}

LogField field(std::string key, std::string value) { return LogField{std::move(key), std::move(value)}; }
LogField field(std::string key, std::uint64_t value) {
  return LogField{std::move(key), std::to_string(value)};
}
LogField field(std::string key, std::int64_t value) {
  return LogField{std::move(key), std::to_string(value)};
}
LogField field(std::string key, bool value) {
  return LogField{std::move(key), value ? "true" : "false"};
}

}  // namespace fum
