#include "fum/engine/verification.hpp"

namespace fum {

json::Value VerificationResult::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("verified", json::Value::make_bool(verified));
  value.set("summary", json::Value::make_string(summary));
  value.set("freshness", freshness.to_json());
  json::Value failures_json = json::Value::make_array();
  for (const auto& failure : failures) {
    failures_json.push(json::Value::make_string(failure));
  }
  value.set("failures", std::move(failures_json));
  return value;
}

VerificationResult assess_verification(const VerificationInput& input) {
  VerificationResult result;
  if (input.artifact == nullptr || input.policy == nullptr) {
    result.summary = "verification requires an artifact and a policy";
    result.failures.push_back("missing-input");
    return result;
  }
  const ArtifactDescriptor& artifact = *input.artifact;
  const VerifyOutcome& outcome = input.outcome;

  if (!outcome.verified) {
    result.failures.push_back("adapter did not confirm the activation: " + outcome.detail);
  }
  if (outcome.observed_version != artifact.version()) {
    result.failures.push_back("observed version " + outcome.observed_version.text() +
                              " does not match the artifact version " + artifact.version().text());
  }
  if (!artifact.build().empty() && !outcome.observed_build.empty() &&
      outcome.observed_build != artifact.build()) {
    result.failures.push_back("observed build " + outcome.observed_build.str() +
                              " does not match the artifact build " + artifact.build().str());
  }
  if (outcome.observed_build.empty()) {
    result.failures.push_back("the adapter did not report a build identity after activation");
  }
  result.freshness = evaluate_freshness(outcome.observed_at, outcome.validity, outcome.incarnation,
                                        input.now, input.current_incarnation);
  if (!result.freshness.fresh) {
    result.failures.push_back("verification evidence is not fresh: " + result.freshness.reason);
  }
  if (input.require_health) {
    if (!outcome.healthy) {
      result.failures.push_back("the adapter reported the target as unhealthy after activation");
    }
    if (input.has_health_evidence()) {
      const FreshnessVerdict health_freshness =
          evaluate_freshness(input.health.observed_at, input.health.validity,
                             input.health.incarnation, input.now, input.current_incarnation);
      if (!health_freshness.fresh) {
        result.failures.push_back("health evidence is not fresh: " + health_freshness.reason);
      }
      if (!input.health.healthy) {
        result.failures.push_back("the health gate reports the target as unhealthy: " +
                                  input.health.detail);
      }
    }
  }
  result.verified = result.failures.empty();
  if (result.verified) {
    result.summary = "post-activation verification passed for " + outcome.observed_version.text() +
                     " (build " + outcome.observed_build.str() + ")";
  } else {
    result.summary = "post-activation verification failed with " +
                     std::to_string(result.failures.size()) + " finding(s)";
  }
  return result;
}

}  // namespace fum
