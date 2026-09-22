// Post-activation verification. Installer exit is never success on its own.
#pragma once

#include <string>
#include <vector>

#include "fum/model/campaign.hpp"
#include "fum/model/policy.hpp"
#include "fum/ports/ports.hpp"

namespace fum {

struct [[nodiscard]] VerificationInput {
  const AttemptRecord* attempt = nullptr;
  const ArtifactDescriptor* artifact = nullptr;
  VerifyOutcome outcome;
  const Policy* policy = nullptr;
  Timestamp now;
  Incarnation current_incarnation;
  bool require_health = true;
  HealthReport health;

  [[nodiscard]] bool has_health_evidence() const {
    return health.observed_at.unix_nanos() != 0;
  }
};

struct [[nodiscard]] VerificationResult {
  bool verified = false;
  FreshnessVerdict freshness;
  std::vector<std::string> failures;
  std::string summary;

  [[nodiscard]] json::Value to_json() const;
};

[[nodiscard]] VerificationResult assess_verification(const VerificationInput& input);

}  // namespace fum
