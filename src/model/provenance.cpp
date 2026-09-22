#include "fum/model/provenance.hpp"

namespace fum {

const char* provenance_outcome_name(ProvenanceOutcome outcome) noexcept {
  switch (outcome) {
    case ProvenanceOutcome::verified: return "verified";
    case ProvenanceOutcome::applied: return "applied";
    case ProvenanceOutcome::rolled_back: return "rolled-back";
    case ProvenanceOutcome::failed: return "failed";
    case ProvenanceOutcome::aborted: return "aborted";
    case ProvenanceOutcome::superseded: return "superseded";
  }
  return "unknown";
}

Result<ProvenanceOutcome> parse_provenance_outcome(std::string_view text) {
  if (text == "verified") return ProvenanceOutcome::verified;
  if (text == "applied") return ProvenanceOutcome::applied;
  if (text == "rolled-back") return ProvenanceOutcome::rolled_back;
  if (text == "failed") return ProvenanceOutcome::failed;
  if (text == "aborted") return ProvenanceOutcome::aborted;
  if (text == "superseded") return ProvenanceOutcome::superseded;
  return make_error(ErrorCode::invalid_argument, "unknown provenance outcome", std::string(text));
}

json::Value ProvenanceRecord::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("record_id", json::Value::make_string(id.str()));
  value.set("sequence", json::Value::make_uint(sequence.value()));
  value.set("campaign_id", json::Value::make_string(campaign.str()));
  value.set("generation", json::Value::make_uint(generation.value()));
  value.set("stage_id", json::Value::make_string(stage.str()));
  value.set("attempt_id", json::Value::make_string(attempt.str()));
  value.set("target_id", json::Value::make_string(target.str()));
  value.set("component_id", json::Value::make_string(component.str()));
  value.set("artifact_id", json::Value::make_string(artifact.str()));
  value.set("build_id", json::Value::make_string(build.str()));
  value.set("from_version", json::Value::make_string(from_version.text()));
  value.set("to_version", json::Value::make_string(to_version.text()));
  value.set("digest", json::Value::make_string(digest.hex()));
  value.set("outcome", json::Value::make_string(provenance_outcome_name(outcome)));
  value.set("authority", json::Value::make_string(authority.str()));
  value.set("incarnation", json::Value::make_uint(incarnation.value()));
  value.set("recorded_at", json::Value::make_string(recorded_at.to_iso8601()));
  value.set("previous_chain", json::Value::make_string(previous.hex()));
  value.set("chain", json::Value::make_string(chain.hex()));
  value.set("detail", json::Value::make_string(detail));
  return value;
}

Result<ProvenanceRecord> ProvenanceRecord::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "provenance record must be a json object");
  }
  ProvenanceRecord out;
  std::string text;
  FUM_TRY(text, value.require_string("record_id"));
  FUM_TRY(out.id, RecordId::parse(text));
  std::uint64_t sequence = 0;
  FUM_TRY(sequence, value.require_uint("sequence"));
  out.sequence = Sequence(sequence);
  FUM_TRY(text, value.require_string("campaign_id"));
  FUM_TRY(out.campaign, CampaignId::parse(text));
  std::uint64_t generation = 0;
  FUM_TRY(generation, value.require_uint("generation"));
  out.generation = Generation(generation);
  FUM_TRY(text, value.require_string("stage_id"));
  FUM_TRY(out.stage, StageId::parse(text));
  FUM_TRY(text, value.require_string("attempt_id"));
  FUM_TRY(out.attempt, AttemptId::parse(text));
  FUM_TRY(text, value.require_string("target_id"));
  FUM_TRY(out.target, TargetId::parse(text));
  FUM_TRY(text, value.require_string("component_id"));
  FUM_TRY(out.component, ComponentId::parse(text));
  FUM_TRY(text, value.require_string("artifact_id"));
  FUM_TRY(out.artifact, ArtifactId::parse(text));
  FUM_TRY(text, value.require_string("build_id"));
  FUM_TRY(out.build, BuildId::parse(text));
  std::string_view version_text;
  FUM_TRY(version_text, value.require_string("from_version"));
  FUM_TRY(out.from_version, Version::parse(version_text));
  FUM_TRY(version_text, value.require_string("to_version"));
  FUM_TRY(out.to_version, Version::parse(version_text));
  std::string_view digest_text;
  FUM_TRY(digest_text, value.require_string("digest"));
  FUM_TRY(out.digest, Digest::parse(digest_text));
  std::string_view outcome_text;
  FUM_TRY(outcome_text, value.require_string("outcome"));
  FUM_TRY(out.outcome, parse_provenance_outcome(outcome_text));
  FUM_TRY(text, value.require_string("authority"));
  FUM_TRY(out.authority, AuthorityId::parse(text));
  std::uint64_t incarnation = 0;
  FUM_TRY(incarnation, value.require_uint("incarnation"));
  out.incarnation = Incarnation(incarnation);
  std::string_view recorded_text;
  FUM_TRY(recorded_text, value.require_string("recorded_at"));
  FUM_TRY(out.recorded_at, Timestamp::parse_iso8601(recorded_text));
  FUM_TRY(digest_text, value.require_string("previous_chain"));
  FUM_TRY(out.previous, Digest::parse(digest_text));
  FUM_TRY(digest_text, value.require_string("chain"));
  FUM_TRY(out.chain, Digest::parse(digest_text));
  const json::Value* detail = value.find("detail");
  if (detail != nullptr) {
    std::string_view detail_text;
    FUM_TRY(detail_text, detail->as_string());
    out.detail = std::string(detail_text);
  }
  return out;
}

Digest ProvenanceRecord::compute_chain(const Digest& previous_chain) const {
  json::Value material = to_json();
  material.erase("previous_chain");
  material.erase("chain");
  return Digest::of_text(previous_chain.hex() + "|" + material.dump());
}

LedgerVerification verify_ledger(const std::vector<ProvenanceRecord>& records) {
  LedgerVerification result;
  result.valid = true;
  result.records = records.size();
  Digest previous;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& record = records[i];
    if (i != 0 && record.sequence.value() <= records[i - 1].sequence.value()) {
      result.valid = false;
      result.first_bad_index = i;
      result.reason = "provenance sequence is not strictly increasing";
      return result;
    }
    if (record.previous != previous) {
      result.valid = false;
      result.first_bad_index = i;
      result.reason = "provenance chain link does not match the preceding record";
      return result;
    }
    const Digest expected = record.compute_chain(previous);
    if (expected != record.chain) {
      result.valid = false;
      result.first_bad_index = i;
      result.reason = "provenance record content does not match its chain digest";
      return result;
    }
    previous = record.chain;
  }
  result.reason = result.valid ? "ledger verified" : "ledger verification failed";
  return result;
}

}  // namespace fum
