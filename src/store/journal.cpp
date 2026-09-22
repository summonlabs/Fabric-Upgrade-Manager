#include "fum/store/journal.hpp"

#include <array>
#include <cstring>

#include "fum/core/checked.hpp"
#include "fum/core/crc.hpp"
#include "fum/core/hash.hpp"

namespace fum {
namespace {

constexpr char kFileMagic[8] = {'F', 'U', 'M', 'J', 'R', 'N', 'L', '1'};
constexpr char kRecordMagic[4] = {'F', 'U', 'M', 'R'};
constexpr std::uint32_t kFormatVersion = 1;
// File header: 8-byte magic, u32 version, u32 flags, i64 creation time.
constexpr std::size_t kHeaderBytes = 8 + 4 + 4 + 8;
constexpr std::size_t kRecordPrefixBytes = 4 + 4 + 4 + hash::kSha256Bytes;

void put_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<char>((value >> 16) & 0xFFu));
  out.push_back(static_cast<char>((value >> 24) & 0xFFu));
}

void put_u64(std::string& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((value >> (i * 8)) & 0xFFu));
  }
}

std::uint32_t read_u32(const std::string& buffer, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[offset])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[offset + 3])) << 24);
}

std::int64_t read_i64(const std::string& buffer, std::size_t offset) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(buffer[offset + static_cast<std::size_t>(i)]))
             << (i * 8);
  }
  return static_cast<std::int64_t>(value);
}

}  // namespace

const char* journal_record_type_name(JournalRecordType type) noexcept {
  switch (type) {
    case JournalRecordType::boot: return "boot";
    case JournalRecordType::epoch: return "epoch";
    case JournalRecordType::policy: return "policy";
    case JournalRecordType::campaign: return "campaign";
    case JournalRecordType::attempt: return "attempt";
    case JournalRecordType::provenance: return "provenance";
    case JournalRecordType::decision: return "decision";
    case JournalRecordType::ticket: return "ticket";
    case JournalRecordType::note: return "note";
    case JournalRecordType::shutdown: return "shutdown";
  }
  return "unknown";
}

Result<JournalRecordType> parse_journal_record_type(std::string_view text) {
  for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(JournalRecordType::shutdown); ++i) {
    const auto type = static_cast<JournalRecordType>(i);
    if (text == journal_record_type_name(type)) {
      return type;
    }
  }
  return make_error(ErrorCode::invalid_argument, "unknown journal record type", std::string(text));
}

json::Value RecoveryReport::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("records_replayed", json::Value::make_uint(records_replayed));
  value.set("truncated_tail", json::Value::make_bool(truncated_tail));
  value.set("bytes_ignored", json::Value::make_uint(bytes_ignored));
  value.set("chain_valid", json::Value::make_bool(chain_valid));
  value.set("sequence_valid", json::Value::make_bool(sequence_valid));
  value.set("clean_shutdown", json::Value::make_bool(clean_shutdown));
  value.set("empty", json::Value::make_bool(empty));
  value.set("detail", json::Value::make_string(detail));
  value.set("damage_reason", json::Value::make_string(damage_reason));
  value.set("last_sequence", json::Value::make_uint(last_sequence.value()));
  value.set("last_boot_incarnation", json::Value::make_uint(last_boot_incarnation.value()));
  value.set("last_epoch", json::Value::make_uint(last_epoch.value()));
  value.set("damaged_records", json::Value::make_uint(damaged_records));
  value.set("rejected_records", json::Value::make_uint(rejected_records));
  value.set("rejected_reason", json::Value::make_string(rejected_reason));
  return value;
}

Journal::Journal(Journal&& other) noexcept
    : path_(std::move(other.path_)),
      options_(other.options_),
      writer_(std::move(other.writer_)),
      records_(std::move(other.records_)),
      recovery_(other.recovery_),
      chain_(other.chain_),
      last_sequence_(other.last_sequence_),
      open_(other.open_),
      tail_damaged_(other.tail_damaged_) {
  other.open_ = false;
  other.tail_damaged_ = false;
}

Journal& Journal::operator=(Journal&& other) noexcept {
  if (this != &other) {
    path_ = std::move(other.path_);
    options_ = other.options_;
    writer_ = std::move(other.writer_);
    records_ = std::move(other.records_);
    recovery_ = other.recovery_;
    chain_ = other.chain_;
    last_sequence_ = other.last_sequence_;
    open_ = other.open_;
    tail_damaged_ = other.tail_damaged_;
    other.open_ = false;
    other.tail_damaged_ = false;
  }
  return *this;
}

Journal::~Journal() {
  if (open_) {
    static_cast<void>(close());
  }
}

Result<Journal> Journal::open(const std::string& path, const JournalOptions& options) {
  Journal journal;
  journal.path_ = path;
  journal.options_ = options;
  if (options.max_record_bytes < 1024 || options.max_record_bytes > (1ull << 30)) {
    return make_error(ErrorCode::invalid_argument, "max_record_bytes is out of range");
  }
  if (options.max_file_bytes < options.max_record_bytes) {
    return make_error(ErrorCode::invalid_argument, "max_file_bytes must cover one record");
  }
  if (options.max_records == 0) {
    return make_error(ErrorCode::invalid_argument, "max_records must be positive");
  }

  std::string existing;
  if (fs::exists(path)) {
    FUM_TRY(existing, fs::read_file(path, options.max_file_bytes + (1ull << 20)));
  }

  std::size_t offset = 0;
  if (existing.size() >= kHeaderBytes) {
    if (std::memcmp(existing.data(), kFileMagic, 8) != 0) {
      return make_error(ErrorCode::corrupt, "journal file magic does not match", path);
    }
    const std::uint32_t version = read_u32(existing, 8);
    if (version != kFormatVersion) {
      return make_error(ErrorCode::corrupt, "journal format version is not supported",
                        std::to_string(version));
    }
    offset = kHeaderBytes;
  } else if (!existing.empty()) {
    return make_error(ErrorCode::corrupt, "journal file is too short to hold a header", path);
  }

  // Replay with conservative tail handling.
  Digest chain;
  Sequence last_sequence;
  std::size_t replayed = 0;
  std::size_t damaged = 0;
  bool truncated_tail = false;
  bool chain_valid = true;
  bool sequence_valid = true;
  std::string damage_reason;
  const std::size_t total = existing.size();
  while (offset < total) {
    if (total - offset < kRecordPrefixBytes) {
      damage_reason = "record prefix is truncated at offset " + std::to_string(offset);
      truncated_tail = true;
      break;
    }
    if (std::memcmp(existing.data() + offset, kRecordMagic, 4) != 0) {
      damage_reason = "record magic mismatch at offset " + std::to_string(offset);
      damaged += 1;
      truncated_tail = true;
      break;
    }
    const std::uint32_t payload_len = read_u32(existing, offset + 4);
    const std::uint32_t crc = read_u32(existing, offset + 8);
    if (payload_len == 0 || payload_len > options.max_record_bytes) {
      damage_reason = "record length " + std::to_string(payload_len) +
                      " is outside the permitted range at offset " + std::to_string(offset);
      damaged += 1;
      truncated_tail = true;
      break;
    }
    const std::size_t payload_offset = offset + kRecordPrefixBytes;
    if (payload_offset + payload_len > total) {
      damage_reason = "record payload runs past the end of the file at offset " +
                      std::to_string(offset);
      truncated_tail = true;
      break;
    }
    const std::string_view payload(existing.data() + payload_offset, payload_len);
    if (crc::crc32c(payload) != crc) {
      damage_reason = "checksum mismatch at offset " + std::to_string(offset) + " (stored " +
                      std::to_string(crc) + ", computed " + std::to_string(crc::crc32c(payload)) +
                      ", length " + std::to_string(payload_len) + ")";
      damaged += 1;
      truncated_tail = true;
      break;
    }
    Digest record_chain;
    std::array<std::uint8_t, hash::kSha256Bytes> chain_bytes{};
    for (std::size_t i = 0; i < hash::kSha256Bytes; ++i) {
      chain_bytes[i] = static_cast<std::uint8_t>(existing[offset + 12 + i]);
    }
    FUM_TRY(record_chain, Digest::parse(hash::to_hex(chain_bytes)));

    auto parsed = json::parse(payload);
    if (!parsed.has_value()) {
      damage_reason = "record payload is not valid json at offset " + std::to_string(offset) +
                      ": " + parsed.error().to_string();
      damaged += 1;
      truncated_tail = true;
      break;
    }
    json::Value record_json = std::move(parsed).value();
    std::uint64_t sequence_value = 0;
    {
      auto sequence_result = record_json.require_uint("seq");
      if (!sequence_result.has_value()) {
        damage_reason = "record has no sequence number at offset " + std::to_string(offset);
        damaged += 1;
        truncated_tail = true;
        break;
      }
      sequence_value = sequence_result.value();
    }
    std::string_view type_text;
    {
      auto type_result = record_json.require_string("type");
      if (!type_result.has_value()) {
        damage_reason = "record has no type at offset " + std::to_string(offset);
        damaged += 1;
        truncated_tail = true;
        break;
      }
      type_text = type_result.value();
    }
    JournalRecordType record_type = JournalRecordType::note;
    {
      auto type_result = parse_journal_record_type(type_text);
      if (!type_result.has_value()) {
        damage_reason = "record type is unknown at offset " + std::to_string(offset) + ": " +
                        std::string(type_text);
        damaged += 1;
        truncated_tail = true;
        break;
      }
      record_type = type_result.value();
    }
    std::string_view at_text;
    {
      auto at_result = record_json.require_string("at");
      if (!at_result.has_value()) {
        damage_reason = "record has no timestamp at offset " + std::to_string(offset);
        damaged += 1;
        truncated_tail = true;
        break;
      }
      at_text = at_result.value();
    }
    Timestamp at;
    {
      auto at_result = Timestamp::parse_iso8601(at_text);
      if (!at_result.has_value()) {
        damage_reason = "record timestamp is malformed at offset " + std::to_string(offset) + ": " +
                        std::string(at_text);
        damaged += 1;
        truncated_tail = true;
        break;
      }
      at = at_result.value();
    }
    std::string_view prev_text;
    {
      auto prev_result = record_json.require_string("prev_chain");
      if (!prev_result.has_value()) {
        damage_reason = "record has no chain link at offset " + std::to_string(offset);
        damaged += 1;
        truncated_tail = true;
        break;
      }
      prev_text = prev_result.value();
    }
    Digest previous;
    {
      auto previous_result = Digest::parse(prev_text);
      if (!previous_result.has_value()) {
        damage_reason = "record chain link is malformed at offset " + std::to_string(offset);
        damaged += 1;
        truncated_tail = true;
        break;
      }
      previous = previous_result.value();
    }
    if (previous != chain) {
      damage_reason = "chain link mismatch at offset " + std::to_string(offset);
      chain_valid = false;
      damaged += 1;
      truncated_tail = true;
      break;
    }
    const Digest expected_chain = Digest::of_text(chain.hex() + "|" + std::string(payload));
    if (expected_chain != record_chain) {
      damage_reason = "record digest mismatch at offset " + std::to_string(offset);
      chain_valid = false;
      damaged += 1;
      truncated_tail = true;
      break;
    }
    if (replayed != 0 && sequence_value <= last_sequence.value()) {
      damage_reason = "sequence number did not advance at offset " + std::to_string(offset);
      sequence_valid = false;
      damaged += 1;
      truncated_tail = true;
      break;
    }
    JournalRecord record;
    record.sequence = Sequence(sequence_value);
    record.type = record_type;
    record.at = at;
    record.previous_chain = previous;
    record.chain = record_chain;
    const json::Value* data = record_json.find("data");
    record.data = data != nullptr ? *data : json::Value::make_object();
    journal.records_.push_back(std::move(record));
    chain = record_chain;
    last_sequence = Sequence(sequence_value);
    replayed += 1;
    if (replayed > options.max_records) {
      return make_error(ErrorCode::resource_exhausted,
                        "journal holds more records than the configured bound",
                        std::to_string(replayed));
    }
    offset = payload_offset + payload_len;
  }

  journal.chain_ = chain;
  journal.last_sequence_ = last_sequence;
  journal.recovery_.records_replayed = replayed;
  journal.recovery_.truncated_tail = truncated_tail;
  journal.recovery_.bytes_ignored = static_cast<std::uint64_t>(total - offset);
  journal.recovery_.chain_valid = chain_valid;
  journal.recovery_.sequence_valid = sequence_valid;
  journal.recovery_.empty = replayed == 0;
  journal.recovery_.damaged_records = damaged;
  journal.recovery_.damage_reason = damage_reason;
  journal.recovery_.last_sequence = last_sequence;
  for (auto it = journal.records_.rbegin(); it != journal.records_.rend(); ++it) {
    const bool terminal =
        it->type == JournalRecordType::boot || it->type == JournalRecordType::snapshot;
    const json::Value* epoch_field = it->data.find("epoch");
    if (epoch_field != nullptr) {
      auto epoch = epoch_field->as_uint();
      if (epoch.has_value()) {
        journal.recovery_.last_epoch = Epoch(epoch.value());
      }
    }
    const json::Value* incarnation_field = it->data.find("incarnation");
    if (incarnation_field != nullptr) {
      auto incarnation = incarnation_field->as_uint();
      if (incarnation.has_value()) {
        journal.recovery_.last_boot_incarnation = Incarnation(incarnation.value());
      }
    }
    if (terminal) {
      break;
    }
  }
  for (auto it = journal.records_.rbegin(); it != journal.records_.rend(); ++it) {
    if (it->type == JournalRecordType::shutdown) {
      journal.recovery_.clean_shutdown = true;
      break;
    }
    if (it->type == JournalRecordType::boot) {
      break;
    }
  }
  journal.tail_damaged_ = truncated_tail;
  journal.recovery_.detail = truncated_tail
                                 ? "journal tail was damaged or incomplete; the damaged records "
                                   "were ignored"
                                 : "journal replayed cleanly";
  if (!chain_valid) {
    journal.recovery_.detail = "journal hash chain is broken at the first damaged record";
  } else if (!sequence_valid) {
    journal.recovery_.detail = "journal sequence numbers are not strictly increasing";
  }
  if (!journal.recovery_.damage_reason.empty()) {
    journal.recovery_.detail.append("; ");
    journal.recovery_.detail.append(journal.recovery_.damage_reason);
  }

  FUM_TRY(journal.writer_, fs::FileWriter::open_append(path, options.max_file_bytes));
  if (existing.empty()) {
    std::string header(kFileMagic, sizeof(kFileMagic));
    put_u32(header, kFormatVersion);
    put_u32(header, 0);
    put_u64(header, static_cast<std::uint64_t>(SystemClock::instance().now().unix_nanos()));
    FUM_TRYV(journal.writer_.append(header));
    FUM_TRYV(journal.writer_.sync());
  }
  journal.open_ = true;
  return journal;
}

Status Journal::write_record(JournalRecordType type, const json::Value& data, Timestamp at,
                             bool allow_recovery_tail) {
  if (!open_) {
    return make_error(ErrorCode::closed, "journal is not open", path_);
  }
  if (tail_damaged_ && !allow_recovery_tail) {
    return make_error(ErrorCode::corrupt,
                      "journal tail is damaged; refusing to append after the damage", path_);
  }
  if (records_.size() >= options_.max_records) {
    return make_error(ErrorCode::resource_exhausted,
                      "journal reached the configured record bound", path_);
  }
  auto next = last_sequence_.try_next();
  if (!next.has_value()) {
    return next.error();
  }
  json::Value payload = json::Value::make_object();
  payload.set("seq", json::Value::make_uint(next.value().value()));
  payload.set("type", json::Value::make_string(journal_record_type_name(type)));
  payload.set("at", json::Value::make_string(at.to_iso8601()));
  payload.set("prev_chain", json::Value::make_string(chain_.hex()));
  payload.set("data", data);
  const std::string body = payload.dump();
  if (static_cast<std::uint64_t>(body.size()) > options_.max_record_bytes) {
    return make_error(ErrorCode::resource_exhausted, "journal record exceeds the configured bound",
                      std::to_string(body.size()));
  }
  const Digest chain = Digest::of_text(chain_.hex() + "|" + body);

  std::string frame(kRecordMagic, sizeof(kRecordMagic));
  put_u32(frame, static_cast<std::uint32_t>(body.size()));
  put_u32(frame, crc::crc32c(body));
  const auto chain_bytes = hash::from_hex(chain.hex());
  if (!chain_bytes.has_value()) {
    return chain_bytes.error();
  }
  for (const std::uint8_t byte : chain_bytes.value()) {
    frame.push_back(static_cast<char>(byte));
  }
  frame.append(body);

  FUM_TRYV(writer_.append(frame));
  if (options_.sync_on_append) {
    FUM_TRYV(writer_.sync());
  }

  JournalRecord record;
  record.sequence = next.value();
  record.type = type;
  record.at = at;
  record.data = data;
  record.previous_chain = chain_;
  record.chain = chain;
  records_.push_back(std::move(record));
  chain_ = chain;
  last_sequence_ = next.value();
  recovery_.last_sequence = next.value();
  recovery_.records_replayed = records_.size();
  recovery_.empty = false;
  return ok_status();
}

Status Journal::append(JournalRecordType type, const json::Value& data, Timestamp at) {
  // Appending after damage is refused: recovery must run first so the operator
  // sees the damaged state instead of silently extending it.
  return write_record(type, data, at, false);
}

Status Journal::sync() { return writer_.sync(); }

Status Journal::close() {
  if (!open_) {
    return ok_status();
  }
  Status status = ok_status();
  if (!tail_damaged_) {
    json::Value payload = json::Value::make_object();
    payload.set("reason", json::Value::make_string("clean shutdown"));
    status = write_record(JournalRecordType::shutdown, payload,
                          SystemClock::instance().now(), true);
  }
  const Status synced = writer_.sync();
  const Status closed = writer_.close();
  open_ = false;
  if (!status.has_value()) {
    return status;
  }
  if (!synced.has_value()) {
    return synced;
  }
  return closed;
}

}  // namespace fum
