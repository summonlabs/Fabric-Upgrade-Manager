// Inspectable decisions: inputs, evidence, policy, selected action, rejections.
#pragma once

#include <string>
#include <vector>

#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/time.hpp"

namespace fum {

enum class DecisionKind : std::uint8_t {
  plan = 0,
  preflight,
  admit,
  stage,
  activate,
  verify,
  block,
  pause,
  resume,
  abort,
  rollback,
  reconcile,
  fence,
};

[[nodiscard]] const char* decision_kind_name(DecisionKind kind) noexcept;
[[nodiscard]] Result<DecisionKind> parse_decision_kind(std::string_view text);

enum class DecisionOutcome : std::uint8_t { allow = 0, deny, defer };

[[nodiscard]] const char* decision_outcome_name(DecisionOutcome outcome) noexcept;
[[nodiscard]] Result<DecisionOutcome> parse_decision_outcome(std::string_view text);

struct [[nodiscard]] DecisionInput {
  std::string name;
  std::string value;
};

struct [[nodiscard]] DecisionEvidenceRef {
  EvidenceId id;
  std::string source;
  std::string summary;
};

struct [[nodiscard]] RejectedAlternative {
  std::string action;
  std::string reason;
};

struct [[nodiscard]] Decision {
  DecisionId id;
  DecisionKind kind = DecisionKind::plan;
  DecisionOutcome outcome = DecisionOutcome::deny;
  CampaignId campaign;
  Generation generation;
  AuthorityId authority;
  Incarnation incarnation;
  Epoch epoch;
  PolicyId policy;
  Revision policy_revision;
  std::vector<DecisionInput> inputs;
  std::vector<DecisionEvidenceRef> evidence;
  std::string selected;
  std::vector<RejectedAlternative> rejected;
  std::string rationale;
  Timestamp at;

  // Deterministic fingerprint over everything except the id, the timestamp and
  // the observing incarnation: identical inputs and policy give an identical
  // fingerprint on any host.
  [[nodiscard]] std::string fingerprint() const;
  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<Decision> from_json(const json::Value& value);
  [[nodiscard]] std::string explain() const;
};

}  // namespace fum
