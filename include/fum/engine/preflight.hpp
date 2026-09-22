// Preflight: resolve inventory, query compatibility, verify integrity, decide.
//
// Preflight never executes anything. It produces a report and, when every gate
// passes, a ticket bound to the campaign generation and the process
// incarnation.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "fum/model/campaign.hpp"
#include "fum/model/policy.hpp"
#include "fum/ports/ports.hpp"

namespace fum {

struct [[nodiscard]] PreflightInput {
  const CampaignRecord* campaign = nullptr;
  const UpgradePlan* plan = nullptr;
  const Policy* policy = nullptr;
  InventorySnapshot inventory;
  IntegrityAssessment integrity;
  ArtifactPayload payload;
  std::map<std::string, CompatibilityVerdict> verdicts;   // keyed by target id
  Timestamp now;
  Incarnation incarnation;
  Epoch epoch;
  bool registry_integrated = false;
  bool drain_integrated = false;
  bool artifact_source_integrated = false;
  bool compatibility_evidence_fresh = true;
};

struct [[nodiscard]] PreflightReport {
  CampaignId campaign;
  Generation generation;
  Incarnation incarnation;
  Epoch epoch;
  AuthorityId authority;
  bool passed = false;
  Timestamp issued_at;
  Timestamp expires_at;
  IntegrityAssessment integrity;
  std::vector<PreflightTargetVerdict> targets;
  std::vector<std::string> blockers;
  SkewAssessment skew;
  std::string fingerprint;
  std::string policy;
  Revision policy_revision;

  [[nodiscard]] PreflightTicket ticket() const;
  [[nodiscard]] json::Value to_json() const;
};

[[nodiscard]] PreflightReport run_preflight(const PreflightInput& input);

}  // namespace fum
