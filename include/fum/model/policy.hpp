// Deterministic operating policy. Every decision names the policy revision it
// was made under.
#pragma once

#include <cstdint>
#include <string>

#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/time.hpp"
#include "fum/model/skew.hpp"

namespace fum {

struct [[nodiscard]] Policy {
  PolicyId id;
  Revision revision;
  std::string name = "default";

  Duration inventory_validity = Duration::from_seconds(30);
  Duration artifact_validity = Duration::from_seconds(300);
  Duration compatibility_validity = Duration::from_seconds(300);
  Duration health_validity = Duration::from_seconds(30);
  Duration verification_validity = Duration::from_seconds(60);
  Duration preflight_validity = Duration::from_seconds(120);
  Duration attempt_lease = Duration::from_seconds(300);

  std::uint32_t max_attempts_per_target = 3;
  std::uint32_t max_retries_per_operation = 2;
  std::uint32_t max_stages = 64;
  std::uint32_t max_targets = 512;
  std::uint32_t max_concurrent_activations = 4;
  std::uint32_t max_decisions_per_campaign = 512;
  std::uint32_t max_operations_per_attempt = 32;
  std::uint32_t max_provenance_records = 100000;

  bool require_signature_declaration = true;
  bool require_health_gate = true;
  bool allow_irreversible_acknowledgement = true;
  bool stop_on_first_stage_failure = true;
  SkewBudget default_skew_budget;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<Policy> from_json(const json::Value& value);
  [[nodiscard]] std::string fingerprint() const;
};

}  // namespace fum
