// Versioned, integrity-checked append-only journal.
//
// Record layout (little endian):
//   file header : "FUMJRNL1" | u32 version | u32 flags | i64 created_ns
//   record      : "FUMR" | u32 payload_len | u32 crc32c(payload) | chain[32] | payload
//   payload     : canonical JSON { seq, type, at, prev_chain, data }
//
// A record is accepted only when its magic, length bound, CRC, chain link and
// sequence number are all valid. Recovery stops at the first problem and keeps
// everything before it; nothing after a damaged record is silently trusted.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fum/core/fs.hpp"
#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/time.hpp"

namespace fum {

enum class JournalRecordType : std::uint8_t {
  boot = 0,
  epoch,
  policy,
  campaign,
  attempt,
  provenance,
  decision,
  ticket,
  note,
  snapshot,
  shutdown,
};

[[nodiscard]] const char* journal_record_type_name(JournalRecordType type) noexcept;
[[nodiscard]] Result<JournalRecordType> parse_journal_record_type(std::string_view text);

struct [[nodiscard]] JournalOptions {
  std::uint64_t max_record_bytes = 1ull * 1024ull * 1024ull;
  std::uint64_t max_file_bytes = 512ull * 1024ull * 1024ull;
  std::size_t max_records = 2000000;
  bool sync_on_append = true;
};

struct [[nodiscard]] JournalRecord {
  Sequence sequence;
  JournalRecordType type = JournalRecordType::note;
  Timestamp at;
  json::Value data;
  Digest previous_chain;
  Digest chain;
};

struct [[nodiscard]] RecoveryReport {
  std::size_t records_replayed = 0;
  bool truncated_tail = false;
  std::uint64_t bytes_ignored = 0;
  bool chain_valid = true;
  bool sequence_valid = true;
  bool clean_shutdown = false;
  bool empty = true;
  std::string detail;
  // Why replay stopped early, if it did: offset and cause.
  std::string damage_reason;
  Sequence last_sequence;
  Incarnation last_boot_incarnation;
  Epoch last_epoch;
  std::size_t damaged_records = 0;
  // Records that replayed structurally but could not be applied to state.
  std::size_t rejected_records = 0;
  std::string rejected_reason;

  [[nodiscard]] json::Value to_json() const;
};

class [[nodiscard]] Journal {
 public:
  Journal() = default;
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  Journal(Journal&& other) noexcept;
  Journal& operator=(Journal&& other) noexcept;
  ~Journal();

  [[nodiscard]] static Result<Journal> open(const std::string& path,
                                            const JournalOptions& options = {});

  [[nodiscard]] Status append(JournalRecordType type, const json::Value& data, Timestamp at);
  [[nodiscard]] const std::vector<JournalRecord>& records() const noexcept { return records_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] Status sync();
  [[nodiscard]] Status close();
  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] Sequence last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  [[nodiscard]] Status write_record(JournalRecordType type, const json::Value& data,
                                    Timestamp at, bool allow_recovery_tail);

  std::string path_;
  JournalOptions options_;
  fs::FileWriter writer_;
  std::vector<JournalRecord> records_;
  RecoveryReport recovery_;
  Digest chain_;
  Sequence last_sequence_;
  bool open_ = false;
  bool tail_damaged_ = false;
};

}  // namespace fum
