// Durable upgrade state: journal-backed campaign, attempt, provenance and
// decision storage with conservative recovery.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "fum/model/campaign.hpp"
#include "fum/store/journal.hpp"

namespace fum {

struct [[nodiscard]] StoreOptions {
  std::string directory;
  JournalOptions journal;
  std::size_t max_campaigns = 4096;
  std::size_t max_attempts = 262144;
  std::size_t max_provenance_records = 100000;
  std::size_t max_decisions_per_campaign = 512;
  // When the journal tail is damaged, opening fails unless the operator
  // explicitly authorises dropping the bytes that could not be verified. Seeing
  // the damage first is deliberate: repair is never silent.
  bool repair_damaged_tail = false;
};

struct [[nodiscard]] StoreStats {
  std::size_t campaigns = 0;
  std::size_t attempts = 0;
  std::size_t provenance_records = 0;
  std::size_t decisions = 0;
  std::uint64_t journal_records = 0;
  Epoch epoch;
  Incarnation incarnation;
  Sequence provenance_sequence;

  [[nodiscard]] json::Value to_json() const;
};

class [[nodiscard]] DurableStore {
 public:
  DurableStore() = default;
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  DurableStore(DurableStore&&) = default;
  DurableStore& operator=(DurableStore&&) = default;

  // Opens (creating if needed) the journal in options.directory, replays it and
  // allocates a fresh process incarnation.
  [[nodiscard]] static Result<DurableStore> open(const StoreOptions& options);

  // Replays the journal again and re-derives in-memory state. Used by tests and
  // by reopen; the first call happens inside open().
  [[nodiscard]] Result<RecoveryReport> recover();

  [[nodiscard]] Status put_campaign(const CampaignRecord& record, Timestamp at);
  [[nodiscard]] Status put_attempt(const AttemptRecord& record, Timestamp at);
  [[nodiscard]] Status append_provenance(ProvenanceRecord& record, Timestamp at);
  [[nodiscard]] Status append_decision(const Decision& decision, Timestamp at);
  [[nodiscard]] Status put_ticket(const PreflightTicket& ticket, Timestamp at);
  [[nodiscard]] Status put_policy(const Policy& policy, Timestamp at);
  [[nodiscard]] Status set_epoch(Epoch epoch, Timestamp at);
  [[nodiscard]] Status note(std::string key, std::string value, Timestamp at);

  // Compaction keeps the journal bounded. The new journal opens with the same
  // hash chain head, so continuity is provable across compaction.
  [[nodiscard]] Status compact(Timestamp at);

  [[nodiscard]] const CampaignRecord* campaign(const CampaignId& id) const;
  [[nodiscard]] const AttemptRecord* attempt(const AttemptId& id) const;
  [[nodiscard]] std::vector<CampaignRecord> campaigns() const;
  [[nodiscard]] std::vector<AttemptRecord> attempts() const;
  [[nodiscard]] std::vector<AttemptRecord> attempts_for(const CampaignId& id) const;
  [[nodiscard]] const std::vector<ProvenanceRecord>& provenance() const noexcept {
    return provenance_;
  }
  [[nodiscard]] const std::vector<Decision>* decisions(const CampaignId& id) const;
  [[nodiscard]] const Policy* policy(const PolicyId& id) const;
  [[nodiscard]] const PreflightTicket* ticket(const CampaignId& id) const;

  [[nodiscard]] Epoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] Incarnation incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const StoreStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const Digest& provenance_chain() const noexcept { return provenance_chain_; }
  [[nodiscard]] Sequence provenance_sequence() const noexcept { return provenance_sequence_; }
  [[nodiscard]] const std::string& journal_path() const noexcept { return journal_.path(); }
  [[nodiscard]] bool tail_damaged() const noexcept { return journal_.recovery().truncated_tail; }

  [[nodiscard]] Status sync();
  [[nodiscard]] Status close();

 private:
  void apply_record(const JournalRecord& record);
  void note_rejected(const JournalRecord& record, const Error& error);
  void rebuild_stats();
  [[nodiscard]] Status ensure_capacity(JournalRecordType type) const;

  StoreOptions options_;
  Journal journal_;
  RecoveryReport recovery_;
  std::unordered_map<std::string, CampaignRecord> campaigns_;
  std::unordered_map<std::string, AttemptRecord> attempts_;
  std::map<std::string, std::vector<Decision>> decisions_;
  std::unordered_map<std::string, Policy> policies_;
  std::unordered_map<std::string, PreflightTicket> tickets_;
  std::vector<ProvenanceRecord> provenance_;
  Digest provenance_chain_;
  Sequence provenance_sequence_;
  Epoch epoch_{1};
  Incarnation incarnation_{1};
  StoreStats stats_;
  std::size_t rejected_records_ = 0;
  std::string rejected_reason_;
};

}  // namespace fum
