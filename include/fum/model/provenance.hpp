// Provenance ledger: which artifact was applied to which target, under which
// campaign generation and authority.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/time.hpp"
#include "fum/model/version.hpp"

namespace fum {

enum class ProvenanceOutcome : std::uint8_t {
  verified = 0,
  applied,
  rolled_back,
  failed,
  aborted,
  superseded,
};

[[nodiscard]] const char* provenance_outcome_name(ProvenanceOutcome outcome) noexcept;
[[nodiscard]] Result<ProvenanceOutcome> parse_provenance_outcome(std::string_view text);

struct [[nodiscard]] ProvenanceRecord {
  RecordId id;
  Sequence sequence;
  CampaignId campaign;
  Generation generation;
  StageId stage;
  AttemptId attempt;
  TargetId target;
  ComponentId component;
  ArtifactId artifact;
  BuildId build;
  Version from_version;
  Version to_version;
  Digest digest;
  ProvenanceOutcome outcome = ProvenanceOutcome::applied;
  AuthorityId authority;
  Incarnation incarnation;
  Timestamp recorded_at;
  Digest previous;   // previous record digest (ledger chain)
  Digest chain;      // digest of this record including previous
  std::string detail;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<ProvenanceRecord> from_json(const json::Value& value);

  // Deterministic material used for chaining; excludes nothing, so any edit is
  // detectable.
  [[nodiscard]] Digest compute_chain(const Digest& previous_chain) const;
};

// Verifies an ordered ledger: sequence monotonicity and chain continuity.
struct [[nodiscard]] LedgerVerification {
  bool valid = false;
  std::size_t records = 0;
  std::size_t first_bad_index = 0;
  std::string reason;
};

[[nodiscard]] LedgerVerification verify_ledger(const std::vector<ProvenanceRecord>& records);

}  // namespace fum
