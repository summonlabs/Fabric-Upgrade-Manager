#include "fum/store/state.hpp"

#include <algorithm>

#include "fum/core/fs.hpp"

namespace fum {

json::Value StoreStats::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("campaigns", json::Value::make_uint(campaigns));
  value.set("attempts", json::Value::make_uint(attempts));
  value.set("provenance_records", json::Value::make_uint(provenance_records));
  value.set("decisions", json::Value::make_uint(decisions));
  value.set("journal_records", json::Value::make_uint(journal_records));
  value.set("epoch", json::Value::make_uint(epoch.value()));
  value.set("incarnation", json::Value::make_uint(incarnation.value()));
  value.set("provenance_sequence", json::Value::make_uint(provenance_sequence.value()));
  return value;
}

Result<DurableStore> DurableStore::open(const StoreOptions& options) {
  if (options.directory.empty()) {
    return make_error(ErrorCode::invalid_argument, "store directory is required");
  }
  if (options.max_campaigns == 0 || options.max_attempts == 0 ||
      options.max_provenance_records == 0 || options.max_decisions_per_campaign == 0) {
    return make_error(ErrorCode::invalid_argument, "store bounds must be positive");
  }
  FUM_TRYV(fs::ensure_directory(options.directory));
  DurableStore store;
  store.options_ = options;
  const std::string path = fs::join(options.directory, "journal.fum");
  FUM_TRY(store.journal_, Journal::open(path, options.journal));
  if (store.journal_.recovery().truncated_tail) {
    const RecoveryReport& recovery = store.journal_.recovery();
    if (!options.repair_damaged_tail) {
      return make_error(ErrorCode::corrupt,
                        "the journal tail is damaged and was not trusted; re-run with repair "
                        "authorised to drop the unverifiable bytes",
                        recovery.damage_reason.empty() ? recovery.detail : recovery.damage_reason);
    }
    // Repair: drop exactly the bytes that could not be verified and continue.
    auto size = fs::file_size(path);
    if (!size.has_value()) {
      return size.error();
    }
    if (recovery.bytes_ignored == 0 || recovery.bytes_ignored > size.value()) {
      return make_error(ErrorCode::corrupt, "the journal damage cannot be bounded for repair",
                        std::to_string(recovery.bytes_ignored));
    }
    const std::uint64_t good_bytes = size.value() - recovery.bytes_ignored;
    // Release the file before rewriting it: the journal holds it open, and the
    // repair must not race its own writer.
    FUM_TRYV(store.journal_.close());
    store.journal_ = Journal{};
    std::string bytes;
    FUM_TRY(bytes, fs::read_file(path, options.journal.max_file_bytes + (1ull << 20)));
    FUM_TRYV(fs::write_file_atomic(path, std::string_view(bytes).substr(0, good_bytes)));
    FUM_TRY(store.journal_, Journal::open(path, options.journal));
    if (store.journal_.recovery().truncated_tail) {
      return make_error(ErrorCode::corrupt, "the journal is still damaged after repair");
    }
  }
  FUM_TRYV(store.recover());
  // Allocate a fresh incarnation: strictly increasing across restarts so a
  // previous process can never be mistaken for the current one.
  const Incarnation previous = store.recovery_.last_boot_incarnation;
  auto next = previous.try_next();
  if (!next.has_value()) {
    return next.error();
  }
  store.incarnation_ = next.value();
  json::Value payload = json::Value::make_object();
  payload.set("incarnation", json::Value::make_uint(store.incarnation_.value()));
  payload.set("pid", json::Value::make_string(fs::process_id_string()));
  FUM_TRYV(store.journal_.append(JournalRecordType::boot, payload, SystemClock::instance().now()));
  store.stats_.incarnation = store.incarnation_;
  return store;
}

Result<RecoveryReport> DurableStore::recover() {
  campaigns_.clear();
  attempts_.clear();
  decisions_.clear();
  policies_.clear();
  tickets_.clear();
  provenance_.clear();
  provenance_chain_ = Digest{};
  provenance_sequence_ = Sequence{};
  epoch_ = Epoch(1);
  incarnation_ = Incarnation(1);
  rejected_records_ = 0;
  rejected_reason_.clear();
  for (const auto& record : journal_.records()) {
    apply_record(record);
  }
  const std::size_t rejected = rejected_records_;
  const std::string rejected_reason = rejected_reason_;
  recovery_ = journal_.recovery();
  recovery_.rejected_records = rejected;
  recovery_.rejected_reason = rejected_reason;
  if (rejected > 0) {
    recovery_.detail.append("; ");
    recovery_.detail.append(std::to_string(rejected));
    recovery_.detail.append(" record(s) could not be applied: ");
    recovery_.detail.append(rejected_reason);
  }
  if (recovery_.last_epoch.value() > epoch_.value()) {
    epoch_ = recovery_.last_epoch;
  }
  rebuild_stats();
  return recovery_;
}

void DurableStore::note_rejected(const JournalRecord& record, const Error& error) {
  rejected_records_ += 1;
  if (rejected_reason_.empty()) {
    rejected_reason_ = std::string(journal_record_type_name(record.type)) +
                       " record at sequence " + std::to_string(record.sequence.value()) +
                       " could not be applied: " + error.to_string();
  }
}

void DurableStore::apply_record(const JournalRecord& record) {
  switch (record.type) {
    case JournalRecordType::campaign: {
      const json::Value* campaign_json = record.data.find("campaign");
      if (campaign_json == nullptr) {
        note_rejected(record, make_error(ErrorCode::corrupt, "campaign record has no payload"));
        break;
      }
      auto parsed = CampaignRecord::from_json(*campaign_json);
      if (parsed.has_value()) {
        campaigns_[parsed.value().id.str()] = std::move(parsed).value();
      } else {
        note_rejected(record, parsed.error());
      }
      break;
    }
    case JournalRecordType::attempt: {
      const json::Value* attempt_json = record.data.find("attempt");
      if (attempt_json == nullptr) {
        note_rejected(record, make_error(ErrorCode::corrupt, "attempt record has no payload"));
        break;
      }
      auto parsed = AttemptRecord::from_json(*attempt_json);
      if (parsed.has_value()) {
        attempts_[parsed.value().id.str()] = std::move(parsed).value();
      } else {
        note_rejected(record, parsed.error());
      }
      break;
    }
    case JournalRecordType::provenance: {
      const json::Value* record_json = record.data.find("record");
      if (record_json == nullptr) {
        break;
      }
      auto parsed = ProvenanceRecord::from_json(*record_json);
      if (parsed.has_value()) {
        provenance_chain_ = parsed.value().chain;
        provenance_sequence_ = parsed.value().sequence;
        provenance_.push_back(std::move(parsed).value());
      } else {
        note_rejected(record, parsed.error());
      }
      break;
    }
    case JournalRecordType::decision: {
      const json::Value* decision_json = record.data.find("decision");
      if (decision_json == nullptr) {
        break;
      }
      auto parsed = Decision::from_json(*decision_json);
      if (parsed.has_value()) {
        auto& list = decisions_[parsed.value().campaign.str()];
        list.push_back(std::move(parsed).value());
      } else {
        note_rejected(record, parsed.error());
      }
      break;
    }
    case JournalRecordType::policy: {
      const json::Value* policy_json = record.data.find("policy");
      if (policy_json == nullptr) {
        break;
      }
      auto parsed = Policy::from_json(*policy_json);
      if (parsed.has_value()) {
        policies_[parsed.value().id.str()] = std::move(parsed).value();
      } else {
        note_rejected(record, parsed.error());
      }
      break;
    }
    case JournalRecordType::ticket: {
      const json::Value* ticket_json = record.data.find("ticket");
      if (ticket_json == nullptr) {
        break;
      }
      auto parsed = PreflightTicket::from_json(*ticket_json);
      if (parsed.has_value()) {
        tickets_[parsed.value().campaign.str()] = std::move(parsed).value();
      } else {
        note_rejected(record, parsed.error());
      }
      break;
    }
    case JournalRecordType::epoch: {
      auto value = record.data.require_uint("epoch");
      if (value.has_value()) {
        epoch_ = Epoch(value.value());
      }
      break;
    }
    case JournalRecordType::snapshot: {
      // Compaction snapshot: replaces previously replayed state.
      const json::Value* campaigns = record.data.find("campaigns");
      campaigns_.clear();
      attempts_.clear();
      decisions_.clear();
      provenance_.clear();
      if (campaigns != nullptr && campaigns->is_array()) {
        for (const auto& item : campaigns->items()) {
          auto parsed = CampaignRecord::from_json(item);
          if (parsed.has_value()) {
            campaigns_[parsed.value().id.str()] = std::move(parsed).value();
          }
        }
      }
      const json::Value* attempts = record.data.find("attempts");
      if (attempts != nullptr && attempts->is_array()) {
        for (const auto& item : attempts->items()) {
          auto parsed = AttemptRecord::from_json(item);
          if (parsed.has_value()) {
            attempts_[parsed.value().id.str()] = std::move(parsed).value();
          }
        }
      }
      const json::Value* records = record.data.find("provenance");
      if (records != nullptr && records->is_array()) {
        for (const auto& item : records->items()) {
          auto parsed = ProvenanceRecord::from_json(item);
          if (parsed.has_value()) {
            provenance_.push_back(std::move(parsed).value());
          }
        }
      }
      const json::Value* chain = record.data.find("provenance_chain");
      if (chain != nullptr) {
        auto text = chain->as_string();
        if (text.has_value()) {
          auto digest = Digest::parse(text.value());
          if (digest.has_value()) {
            provenance_chain_ = digest.value();
          }
        }
      }
      const json::Value* sequence = record.data.find("provenance_sequence");
      if (sequence != nullptr) {
        auto value = sequence->as_uint();
        if (value.has_value()) {
          provenance_sequence_ = Sequence(value.value());
        }
      }
      const json::Value* epoch_value = record.data.find("epoch");
      if (epoch_value != nullptr) {
        auto value = epoch_value->as_uint();
        if (value.has_value()) {
          epoch_ = Epoch(value.value());
        }
      }
      const json::Value* boot = record.data.find("incarnation");
      if (boot != nullptr) {
        auto value = boot->as_uint();
        if (value.has_value()) {
          incarnation_ = Incarnation(value.value());
        }
      }
      break;
    }
    case JournalRecordType::boot: {
      auto value = record.data.require_uint("incarnation");
      if (value.has_value()) {
        incarnation_ = Incarnation(value.value());
      }
      break;
    }
    case JournalRecordType::note:
    case JournalRecordType::shutdown:
      break;
  }
}

void DurableStore::rebuild_stats() {
  stats_.campaigns = campaigns_.size();
  stats_.attempts = attempts_.size();
  stats_.provenance_records = provenance_.size();
  std::size_t decisions = 0;
  for (const auto& entry : decisions_) {
    decisions += entry.second.size();
  }
  stats_.decisions = decisions;
  stats_.journal_records = journal_.records().size();
  stats_.epoch = epoch_;
  stats_.incarnation = incarnation_;
  stats_.provenance_sequence = provenance_sequence_;
}

Status DurableStore::ensure_capacity(JournalRecordType type) const {
  switch (type) {
    case JournalRecordType::campaign:
      if (campaigns_.size() >= options_.max_campaigns) {
        return make_error(ErrorCode::resource_exhausted,
                          "campaign bound reached; compact or retire campaigns",
                          std::to_string(options_.max_campaigns));
      }
      break;
    case JournalRecordType::attempt:
      if (attempts_.size() >= options_.max_attempts) {
        return make_error(ErrorCode::resource_exhausted, "attempt bound reached",
                          std::to_string(options_.max_attempts));
      }
      break;
    case JournalRecordType::provenance:
      if (provenance_.size() >= options_.max_provenance_records) {
        return make_error(ErrorCode::resource_exhausted, "provenance ledger bound reached",
                          std::to_string(options_.max_provenance_records));
      }
      break;
    case JournalRecordType::decision:
      break;
    default:
      break;
  }
  return ok_status();
}

Status DurableStore::put_campaign(const CampaignRecord& record, Timestamp at) {
  const bool exists = campaigns_.find(record.id.str()) != campaigns_.end();
  if (!exists) {
    FUM_TRYV(ensure_capacity(JournalRecordType::campaign));
  }
  json::Value payload = json::Value::make_object();
  payload.set("campaign", record.to_json());
  FUM_TRYV(journal_.append(JournalRecordType::campaign, payload, at));
  campaigns_[record.id.str()] = record;
  rebuild_stats();
  return ok_status();
}

Status DurableStore::put_attempt(const AttemptRecord& record, Timestamp at) {
  const bool exists = attempts_.find(record.id.str()) != attempts_.end();
  if (!exists) {
    FUM_TRYV(ensure_capacity(JournalRecordType::attempt));
  }
  json::Value payload = json::Value::make_object();
  payload.set("attempt", record.to_json());
  FUM_TRYV(journal_.append(JournalRecordType::attempt, payload, at));
  attempts_[record.id.str()] = record;
  rebuild_stats();
  return ok_status();
}

Status DurableStore::append_provenance(ProvenanceRecord& record, Timestamp at) {
  FUM_TRYV(ensure_capacity(JournalRecordType::provenance));
  auto next = provenance_sequence_.try_next();
  if (!next.has_value()) {
    return next.error();
  }
  record.sequence = next.value();
  record.previous = provenance_chain_;
  record.chain = record.compute_chain(provenance_chain_);
  json::Value payload = json::Value::make_object();
  payload.set("record", record.to_json());
  payload.set("provenance_chain", json::Value::make_string(record.chain.hex()));
  payload.set("provenance_sequence", json::Value::make_uint(record.sequence.value()));
  FUM_TRYV(journal_.append(JournalRecordType::provenance, payload, at));
  provenance_.push_back(record);
  provenance_chain_ = record.chain;
  provenance_sequence_ = record.sequence;
  rebuild_stats();
  return ok_status();
}

Status DurableStore::append_decision(const Decision& decision, Timestamp at) {
  auto& list = decisions_[decision.campaign.str()];
  if (list.size() >= options_.max_decisions_per_campaign) {
    list.erase(list.begin());
  }
  json::Value payload = json::Value::make_object();
  payload.set("decision", decision.to_json());
  FUM_TRYV(journal_.append(JournalRecordType::decision, payload, at));
  list.push_back(decision);
  rebuild_stats();
  return ok_status();
}

Status DurableStore::put_ticket(const PreflightTicket& ticket, Timestamp at) {
  json::Value payload = json::Value::make_object();
  payload.set("ticket", ticket.to_json());
  FUM_TRYV(journal_.append(JournalRecordType::ticket, payload, at));
  tickets_[ticket.campaign.str()] = ticket;
  return ok_status();
}

Status DurableStore::put_policy(const Policy& policy, Timestamp at) {
  json::Value payload = json::Value::make_object();
  payload.set("policy", policy.to_json());
  FUM_TRYV(journal_.append(JournalRecordType::policy, payload, at));
  policies_[policy.id.str()] = policy;
  return ok_status();
}

Status DurableStore::set_epoch(Epoch epoch, Timestamp at) {
  if (epoch <= epoch_) {
    return make_error(ErrorCode::fenced, "epoch must advance monotonically",
                      std::to_string(epoch.value()) + " <= " + std::to_string(epoch_.value()));
  }
  json::Value payload = json::Value::make_object();
  payload.set("epoch", json::Value::make_uint(epoch.value()));
  FUM_TRYV(journal_.append(JournalRecordType::epoch, payload, at));
  epoch_ = epoch;
  rebuild_stats();
  return ok_status();
}

Status DurableStore::note(std::string key, std::string value, Timestamp at) {
  json::Value payload = json::Value::make_object();
  payload.set("key", json::Value::make_string(key));
  payload.set("value", json::Value::make_string(value));
  return journal_.append(JournalRecordType::note, payload, at);
}

Status DurableStore::compact(Timestamp at) {
  json::Value snapshot = json::Value::make_object();
  json::Value campaigns_json = json::Value::make_array();
  std::vector<std::string> campaign_ids;
  campaign_ids.reserve(campaigns_.size());
  for (const auto& entry : campaigns_) {
    campaign_ids.push_back(entry.first);
  }
  std::sort(campaign_ids.begin(), campaign_ids.end());
  for (const auto& id : campaign_ids) {
    campaigns_json.push(campaigns_.at(id).to_json());
  }
  snapshot.set("campaigns", std::move(campaigns_json));

  json::Value attempts_json = json::Value::make_array();
  std::vector<std::string> attempt_ids;
  attempt_ids.reserve(attempts_.size());
  for (const auto& entry : attempts_) {
    attempt_ids.push_back(entry.first);
  }
  std::sort(attempt_ids.begin(), attempt_ids.end());
  for (const auto& id : attempt_ids) {
    attempts_json.push(attempts_.at(id).to_json());
  }
  snapshot.set("attempts", std::move(attempts_json));

  json::Value provenance_json = json::Value::make_array();
  for (const auto& record : provenance_) {
    provenance_json.push(record.to_json());
  }
  snapshot.set("provenance", std::move(provenance_json));
  snapshot.set("provenance_chain", json::Value::make_string(provenance_chain_.hex()));
  snapshot.set("provenance_sequence", json::Value::make_uint(provenance_sequence_.value()));
  snapshot.set("epoch", json::Value::make_uint(epoch_.value()));
  snapshot.set("incarnation", json::Value::make_uint(incarnation_.value()));

  const std::string directory = options_.directory;
  const std::string temporary = fs::join(directory, "journal.compact");
  static_cast<void>(fs::remove_file(temporary));

  Journal fresh;
  FUM_TRY(fresh, Journal::open(temporary, options_.journal));
  // The compacted journal continues the existing chain head, so continuity is
  // still provable after compaction.
  FUM_TRYV(fresh.append(JournalRecordType::snapshot, snapshot, at));
  FUM_TRYV(fresh.sync());
  FUM_TRYV(fresh.close());

  const std::string target = journal_.path();
  FUM_TRYV(journal_.close());
  Journal closed;
  journal_ = std::move(closed);
  FUM_TRYV(fs::replace_file(temporary, target));
  FUM_TRY(journal_, Journal::open(target, options_.journal));
  FUM_TRYV(recover());
  return ok_status();
}

const CampaignRecord* DurableStore::campaign(const CampaignId& id) const {
  const auto it = campaigns_.find(id.str());
  return it == campaigns_.end() ? nullptr : &it->second;
}

const AttemptRecord* DurableStore::attempt(const AttemptId& id) const {
  const auto it = attempts_.find(id.str());
  return it == attempts_.end() ? nullptr : &it->second;
}

std::vector<CampaignRecord> DurableStore::campaigns() const {
  std::vector<CampaignRecord> out;
  out.reserve(campaigns_.size());
  for (const auto& entry : campaigns_) {
    out.push_back(entry.second);
  }
  std::sort(out.begin(), out.end(),
            [](const CampaignRecord& a, const CampaignRecord& b) { return a.id < b.id; });
  return out;
}

std::vector<AttemptRecord> DurableStore::attempts() const {
  std::vector<AttemptRecord> out;
  out.reserve(attempts_.size());
  for (const auto& entry : attempts_) {
    out.push_back(entry.second);
  }
  std::sort(out.begin(), out.end(),
            [](const AttemptRecord& a, const AttemptRecord& b) { return a.id < b.id; });
  return out;
}

std::vector<AttemptRecord> DurableStore::attempts_for(const CampaignId& id) const {
  std::vector<AttemptRecord> out;
  for (const auto& entry : attempts_) {
    if (entry.second.campaign == id) {
      out.push_back(entry.second);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const AttemptRecord& a, const AttemptRecord& b) { return a.id < b.id; });
  return out;
}

const std::vector<Decision>* DurableStore::decisions(const CampaignId& id) const {
  const auto it = decisions_.find(id.str());
  return it == decisions_.end() ? nullptr : &it->second;
}

const Policy* DurableStore::policy(const PolicyId& id) const {
  const auto it = policies_.find(id.str());
  return it == policies_.end() ? nullptr : &it->second;
}

const PreflightTicket* DurableStore::ticket(const CampaignId& id) const {
  const auto it = tickets_.find(id.str());
  return it == tickets_.end() ? nullptr : &it->second;
}

Status DurableStore::sync() { return journal_.sync(); }

Status DurableStore::close() { return journal_.close(); }

}  // namespace fum
