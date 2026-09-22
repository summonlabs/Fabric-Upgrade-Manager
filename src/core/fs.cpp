#include "fum/core/fs.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "fum/core/checked.hpp"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fum::fs {
namespace {

std::atomic<std::uint64_t> g_temp_counter{0};

Result<std::filesystem::path> to_path(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::invalid_argument, "path must not be empty");
  }
  if (text.find('\0') != std::string_view::npos) {
    return make_error(ErrorCode::invalid_argument, "path contains a NUL byte");
  }
  return std::filesystem::path(std::string(text));
}

Status from_error(const std::error_code& ec, std::string_view what, std::string_view path) {
  if (!ec) {
    return ok_status();
  }
  return make_error(ErrorCode::io_error, std::string(what), std::string(path) + ": " + ec.message());
}

}  // namespace

std::string join(std::string_view parent, std::string_view child) {
  if (parent.empty()) {
    return std::string(child);
  }
  if (child.empty()) {
    return std::string(parent);
  }
  std::string out(parent);
  const char last = out.back();
  if (last != '/' && last != '\\') {
    out.push_back('/');
  }
  out.append(child);
  return out;
}

bool is_absolute(std::string_view path) {
  if (path.size() >= 2 && path[1] == ':') {
    return true;
  }
  return !path.empty() && (path.front() == '/' || path.front() == '\\');
}

std::string parent_directory(std::string_view path) {
  const std::filesystem::path p{std::string(path)};
  return p.parent_path().string();
}

std::string file_name(std::string_view path) {
  const std::filesystem::path p{std::string(path)};
  return p.filename().string();
}

bool exists(std::string_view path) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(std::string(path)), ec);
}

bool is_directory(std::string_view path) {
  std::error_code ec;
  return std::filesystem::is_directory(std::filesystem::path(std::string(path)), ec);
}

Result<std::uint64_t> file_size(std::string_view path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(std::filesystem::path(std::string(path)), ec);
  if (ec) {
    return make_error(ErrorCode::io_error, "could not stat file",
                      std::string(path) + ": " + ec.message());
  }
  return static_cast<std::uint64_t>(size);
}

Result<std::string> read_file(std::string_view path, std::uint64_t max_bytes) {
  std::error_code ec;
  const std::filesystem::path p{std::string(path)};
  const auto size = std::filesystem::file_size(p, ec);
  if (ec) {
    return make_error(ErrorCode::io_error, "could not stat file for reading",
                      std::string(path) + ": " + ec.message());
  }
  FUM_TRYV(checked::require_bound(static_cast<std::uint64_t>(size), max_bytes, "file size"));
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, std::string(path).c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(std::string(path).c_str(), "rb");
#endif
  if (file == nullptr) {
    return make_error(ErrorCode::io_error, "could not open file for reading", std::string(path));
  }
  std::string data;
  data.resize(static_cast<std::size_t>(size));
  const std::size_t read = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), file);
  const bool short_read = read != data.size();
  std::fclose(file);
  if (short_read) {
    return make_error(ErrorCode::io_error, "short read while reading file", std::string(path));
  }
  return data;
}

Status ensure_directory(std::string_view path) {
  std::error_code ec;
  const auto p = std::filesystem::path(std::string(path));
  if (std::filesystem::exists(p, ec)) {
    if (!std::filesystem::is_directory(p, ec)) {
      return make_error(ErrorCode::io_error, "path exists and is not a directory",
                        std::string(path));
    }
    return ok_status();
  }
  std::filesystem::create_directories(p, ec);
  return from_error(ec, "could not create directory", path);
}

Status remove_file(std::string_view path) {
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(std::string(path)), ec);
  return from_error(ec, "could not remove file", path);
}

Status remove_tree(std::string_view path) {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(std::string(path)), ec);
  return from_error(ec, "could not remove directory tree", path);
}

Status copy_file(std::string_view from, std::string_view to, bool replace_existing) {
  std::error_code ec;
  const auto options = replace_existing ? std::filesystem::copy_options::overwrite_existing
                                        : std::filesystem::copy_options::none;
  std::filesystem::copy_file(std::filesystem::path(std::string(from)),
                             std::filesystem::path(std::string(to)), options, ec);
  return from_error(ec, "could not copy file", std::string(from) + " -> " + std::string(to));
}

Status replace_file(std::string_view from, std::string_view to) {
#if defined(_WIN32)
  const std::wstring wfrom = std::filesystem::path(std::string(from)).wstring();
  const std::wstring wto = std::filesystem::path(std::string(to)).wstring();
  if (!::MoveFileExW(wfrom.c_str(), wto.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return make_error(ErrorCode::io_error, "could not atomically replace file",
                      std::string(to));
  }
  return ok_status();
#else
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(std::string(from)),
                          std::filesystem::path(std::string(to)), ec);
  return from_error(ec, "could not atomically replace file", to);
#endif
}

Status write_file(std::string_view path, std::string_view data) {
  const std::string target(path);
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, target.c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(target.c_str(), "wb");
#endif
  if (file == nullptr) {
    return make_error(ErrorCode::io_error, "could not open file for writing", target);
  }
  if (!data.empty()) {
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), file);
    if (written != data.size()) {
      std::fclose(file);
      return make_error(ErrorCode::io_error, "short write", target);
    }
  }
  std::fflush(file);
#if defined(_WIN32)
  _commit(_fileno(file));
#else
  ::fsync(fileno(file));
#endif
  std::fclose(file);
  return ok_status();
}

Status write_file_atomic(std::string_view path, std::string_view data) {
  const std::string target(path);
  const std::uint64_t counter = g_temp_counter.fetch_add(1);
  const std::string temp =
      target + ".tmp-" + process_id_string() + "-" + std::to_string(counter);
  FUM_TRYV(write_file(temp, data));
  auto replaced = replace_file(temp, target);
  if (!replaced.has_value()) {
    static_cast<void>(remove_file(temp));
    return replaced.error();
  }
  return ok_status();
}

Result<std::vector<std::string>> list_directory(std::string_view path) {
  std::error_code ec;
  const auto p = to_path(path);
  if (!p.has_value()) {
    return p.error();
  }
  std::filesystem::directory_iterator iterator(p.value(), ec);
  if (ec) {
    return make_error(ErrorCode::io_error, "could not list directory",
                      std::string(path) + ": " + ec.message());
  }
  std::vector<std::string> entries;
  for (const auto& entry : iterator) {
    entries.push_back(entry.path().filename().string());
  }
  std::sort(entries.begin(), entries.end());
  return entries;
}

FileWriter::FileWriter(FileWriter&& other) noexcept
    : handle_(other.handle_),
      path_(std::move(other.path_)),
      limit_(other.limit_),
      bytes_written_(other.bytes_written_) {
  other.handle_ = nullptr;
  other.bytes_written_ = 0;
  other.limit_ = 0;
}

FileWriter& FileWriter::operator=(FileWriter&& other) noexcept {
  if (this != &other) {
    reset();
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    limit_ = other.limit_;
    bytes_written_ = other.bytes_written_;
    other.handle_ = nullptr;
    other.bytes_written_ = 0;
    other.limit_ = 0;
  }
  return *this;
}

FileWriter::~FileWriter() { reset(); }

void FileWriter::reset() {
  if (handle_ != nullptr) {
    std::fclose(handle_);
    handle_ = nullptr;
  }
}

Result<FileWriter> FileWriter::open_append(std::string_view path, std::uint64_t max_bytes) {
  FileWriter writer;
  writer.path_ = std::string(path);
  writer.limit_ = max_bytes;
  FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, writer.path_.c_str(), "ab") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(writer.path_.c_str(), "ab");
#endif
  if (file == nullptr) {
    return make_error(ErrorCode::io_error, "could not open file for append", writer.path_);
  }
  writer.handle_ = file;
  std::error_code ec;
  const auto size = std::filesystem::file_size(std::filesystem::path(writer.path_), ec);
  writer.bytes_written_ = ec ? 0 : static_cast<std::uint64_t>(size);
  if (writer.bytes_written_ > max_bytes) {
    std::fclose(file);
    writer.handle_ = nullptr;
    return make_error(ErrorCode::resource_exhausted,
                      "append target already exceeds the configured bound", writer.path_);
  }
  return writer;
}

Status FileWriter::append(std::string_view data) {
  if (handle_ == nullptr) {
    return make_error(ErrorCode::closed, "file writer is not open", path_);
  }
  const auto projected = checked::add_u64(bytes_written_, static_cast<std::uint64_t>(data.size()));
  if (!projected.has_value()) {
    return projected.error();
  }
  if (projected.value() > limit_) {
    return make_error(ErrorCode::resource_exhausted, "append would exceed the configured bound",
                      path_ + ": " + std::to_string(projected.value()) + " > " +
                          std::to_string(limit_));
  }
  if (!data.empty()) {
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), handle_);
    if (written != data.size()) {
      return make_error(ErrorCode::io_error, "short write during append", path_);
    }
  }
  bytes_written_ = projected.value();
  return ok_status();
}

Status FileWriter::flush() {
  if (handle_ == nullptr) {
    return make_error(ErrorCode::closed, "file writer is not open", path_);
  }
  if (std::fflush(handle_) != 0) {
    return make_error(ErrorCode::io_error, "could not flush file", path_);
  }
  return ok_status();
}

Status FileWriter::sync() {
  FUM_TRYV(flush());
#if defined(_WIN32)
  if (_commit(_fileno(handle_)) != 0) {
    return make_error(ErrorCode::io_error, "could not flush file to disk", path_);
  }
#else
  if (::fsync(fileno(handle_)) != 0) {
    return make_error(ErrorCode::io_error, "could not flush file to disk", path_);
  }
#endif
  return ok_status();
}

Status FileWriter::close() {
  if (handle_ == nullptr) {
    return ok_status();
  }
  const Status synced = sync();
  std::fclose(handle_);
  handle_ = nullptr;
  return synced;
}

TempDir::TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)), keep_(other.keep_) {
  other.path_.clear();
  other.keep_ = false;
}

TempDir& TempDir::operator=(TempDir&& other) noexcept {
  if (this != &other) {
    if (!path_.empty() && !keep_) {
      static_cast<void>(remove_tree(path_));
    }
    path_ = std::move(other.path_);
    keep_ = other.keep_;
    other.path_.clear();
    other.keep_ = false;
  }
  return *this;
}

TempDir::~TempDir() {
  if (!path_.empty() && !keep_) {
    static_cast<void>(remove_tree(path_));
  }
}

Result<TempDir> TempDir::create(std::string_view prefix) {
  std::error_code ec;
  const auto base = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return make_error(ErrorCode::io_error, "could not resolve the temporary directory",
                      ec.message());
  }
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::uint64_t counter = g_temp_counter.fetch_add(1);
    const auto candidate =
        base / (std::string(prefix) + "-" + process_id_string() + "-" + std::to_string(counter));
    std::error_code create_ec;
    if (std::filesystem::create_directory(candidate, create_ec)) {
      TempDir dir;
      dir.path_ = candidate.string();
      return dir;
    }
  }
  return make_error(ErrorCode::io_error, "could not create a unique temporary directory",
                    std::string(prefix));
}

std::string process_id_string() {
#if defined(_WIN32)
  return std::to_string(static_cast<unsigned long>(_getpid()));
#else
  return std::to_string(static_cast<long>(::getpid()));
#endif
}

}  // namespace fum::fs
