// Filesystem primitives with explicit durability and bounded reads.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "fum/core/result.hpp"

namespace fum::fs {

inline constexpr std::uint64_t kDefaultMaxFileBytes = 64ull * 1024ull * 1024ull;

[[nodiscard]] std::string join(std::string_view parent, std::string_view child);
[[nodiscard]] bool is_absolute(std::string_view path);
[[nodiscard]] std::string parent_directory(std::string_view path);
[[nodiscard]] std::string file_name(std::string_view path);

[[nodiscard]] bool exists(std::string_view path);
[[nodiscard]] bool is_directory(std::string_view path);
[[nodiscard]] Result<std::uint64_t> file_size(std::string_view path);

// Reads the whole file but refuses to allocate more than max_bytes.
[[nodiscard]] Result<std::string> read_file(std::string_view path,
                                            std::uint64_t max_bytes = kDefaultMaxFileBytes);

[[nodiscard]] Status ensure_directory(std::string_view path);
[[nodiscard]] Status remove_file(std::string_view path);
[[nodiscard]] Status remove_tree(std::string_view path);
[[nodiscard]] Status copy_file(std::string_view from, std::string_view to,
                               bool replace_existing = true);

// Durable writes: temp file + flush + atomic replace.
[[nodiscard]] Status write_file_atomic(std::string_view path, std::string_view data);
[[nodiscard]] Status write_file(std::string_view path, std::string_view data);
[[nodiscard]] Status replace_file(std::string_view from, std::string_view to);

[[nodiscard]] Result<std::vector<std::string>> list_directory(std::string_view path);

// Append-only file with an explicit durability point. Bounded by max_bytes so a
// runaway writer cannot exhaust the volume silently.
class [[nodiscard]] FileWriter {
 public:
  FileWriter() = default;
  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;
  FileWriter(FileWriter&& other) noexcept;
  FileWriter& operator=(FileWriter&& other) noexcept;
  ~FileWriter();

  [[nodiscard]] static Result<FileWriter> open_append(std::string_view path,
                                                      std::uint64_t max_bytes);
  [[nodiscard]] Status append(std::string_view data);
  [[nodiscard]] Status sync();
  [[nodiscard]] Status flush();
  [[nodiscard]] Status close();
  [[nodiscard]] std::uint64_t bytes_written() const noexcept { return bytes_written_; }
  [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }

 private:
  void reset();

  std::FILE* handle_ = nullptr;
  std::string path_;
  std::uint64_t limit_ = 0;
  std::uint64_t bytes_written_ = 0;
};

// Creates a unique directory and removes it on destruction unless kept.
class [[nodiscard]] TempDir {
 public:
  TempDir() = default;
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&& other) noexcept;
  TempDir& operator=(TempDir&& other) noexcept;
  ~TempDir();

  [[nodiscard]] static Result<TempDir> create(std::string_view prefix);
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  void keep() noexcept { keep_ = true; }

 private:
  std::string path_;
  bool keep_ = false;
};

[[nodiscard]] std::string process_id_string();

}  // namespace fum::fs
