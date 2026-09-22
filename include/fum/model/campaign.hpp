// Upgrade plans, stages, attempts and the durable campaign record.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/time.hpp"
#include "fum/model/artifact.hpp"
#include "fum/model/decision.hpp"
#include "fum/model/lifecycle.hpp"
#include "fum/model/policy.hpp"
#include "fum/model/provenance.hpp"
#include "fum/model/skew.hpp"
#include "fum/model/target.hpp"

namespace fum {

struct [[nodiscard]] PlanStep {
  StepId id;
  std::string action;         // prepare | activate | verify | rollback
  std::string description;
  bool irreversible = false;
  std::string irreversible_reason;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<PlanStep> from_json(const json::Value& value);
};

struct [[nodiscard]] StagePlan {
  StageId id;
  std::string name;
  std::vector<TargetId> targets;
  std::vector<PlanStep> steps;
  bool requires_service_removal = false;
  std::string redundancy_group;
  std::uint32_t max_parallel = 1;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<StagePlan> from_json(const json::Value& value);
};

enum class PlanRejectionCode : std::uint8_t {
  no_targets = 0,
  unknown_target,
  mixed_component,
  platform_unsupported,
  capability_missing,
  strategy_unsupported,
  duplicate_target,
  integrity_unverified,
  version_regression,
  skew_unsatisfiable,
  irreversible_unacknowledged,
  target_limit_exceeded,
  stage_limit_exceeded,
  adapter_claim_conflict,
  artifact_component_mismatch,
  already_at_version,
};

[[nodiscard]] const char* plan_rejection_name(PlanRejectionCode code) noexcept;
[[nodiscard]] Result<PlanRejectionCode> parse_plan_rejection(std::string_view text);

struct [[nodiscard]] PlanRejection {
  PlanRejectionCode code = PlanRejectionCode::no_targets;
  std::string subject;
  std::string detail;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<PlanRejection> from_json(const json::Value& value);
};

struct [[nodiscard]] UpgradePlanRequest {
  CampaignId campaign;
  ComponentId component;
  ArtifactDescriptor artifact;
  std::vector<TargetId> targets;   // empty means every target of the component
  StrategyKind strategy = StrategyKind::in_place;
  SkewBudget skew_budget;
  AuthorityId authority;
  bool acknowledge_irreversible_steps = false;
  std::vector<StepId> acknowledged_irreversible_steps;
  std::uint32_t max_parallel_activations = 1;
  std::string description;
  Timestamp requested_at;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<UpgradePlanRequest> from_json(const json::Value& value);
};

struct [[nodiscard]] UpgradePlan {
  CampaignId campaign;
  Generation generation;
  AuthorityId authority;
  ComponentId component;
  ArtifactDescriptor artifact;
  StrategyKind strategy = StrategyKind::in_place;
  std::vector<StagePlan> stages;
  SkewBudget skew_budget;
  std::vector<PlanRejection> rejections;
  std::vector<StepId> irreversible_steps;
  PolicyId policy;
  Revision policy_revision;
  Timestamp created_at;
  std::string description;

  [[nodiscard]] bool executable() const { return rejections.empty(); }
  [[nodiscard]] std::size_t target_count() const;
  [[nodiscard]] std::string fingerprint() const;
  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<UpgradePlan> from_json(const json::Value& value);
};

struct [[nodiscard]] StageProgress {
  StageId id;
  StageState state = StageState::pending;
  std::vector<AttemptId> attempts;
  Timestamp started_at;
  Timestamp completed_at;
  std::string detail;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<StageProgress> from_json(const json::Value& value);
};

struct [[nodiscard]] PreflightTargetVerdict {
  TargetId target;
  bool compatible = false;
  bool integrity_verified = false;
  bool platform_supported = false;
  bool strategy_supported = false;
  bool capabilities_satisfied = false;
  bool rollback_available = false;
  std::vector<std::string> reasons;

  [[nodiscard]] bool admissible() const {
    return compatible && integrity_verified && platform_supported && strategy_supported &&
           capabilities_satisfied;
  }
  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<PreflightTargetVerdict> from_json(const json::Value& value);
};

// A passing preflight is a ticket with a generation and an expiry. Execution
// refuses to proceed without a current ticket.
struct [[nodiscard]] PreflightTicket {
  CampaignId campaign;
  Generation generation;
  Incarnation incarnation;
  Epoch epoch;
  Timestamp issued_at;
  Timestamp expires_at;
  std::string fingerprint;
  bool passed = false;
  AuthorityId authority;
  PolicyId policy;
  Revision policy_revision;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<PreflightTicket> from_json(const json::Value& value);
};

struct [[nodiscard]] CampaignRecord {
  CampaignId id;
  Generation generation;
  Epoch epoch;
  AuthorityId authority;
  ComponentId component;
  UpgradeState state = UpgradeState::proposed;
  StrategyKind strategy = StrategyKind::in_place;
  ArtifactDescriptor artifact;
  UpgradePlan plan;
  std::vector<StageProgress> stages;
  std::size_t active_stage = 0;
  Revision revision;
  Incarnation owner_incarnation;
  Timestamp created_at;
  Timestamp updated_at;
  bool irreversible_acknowledged = false;
  std::vector<StepId> acknowledged_steps;
  std::string block_reason;
  std::string failure_reason;
  std::string pause_reason;
  std::string rollback_plan;
  std::string rollback_artifact;
  bool rollback_eligible = false;
  bool rollback_performed = false;
  std::uint32_t attempts_started = 0;
  std::uint32_t decisions_recorded = 0;
  PreflightTicket ticket;
  // Derived from live attempts and inventory; never persisted, because a
  // deserialized assessment would be stale evidence masquerading as current.
  SkewAssessment last_skew;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<CampaignRecord> from_json(const json::Value& value);
};

struct [[nodiscard]] AttemptRecord {
  AttemptId id;
  CampaignId campaign;
  Generation generation;
  StageId stage;
  TargetId target;
  ArtifactId artifact;
  BuildId build;
  Version from_version;
  Version to_version;
  AttemptState state = AttemptState::created;
  Incarnation owner;
  AuthorityId authority;
  Revision revision;
  Sequence operation_index;
  std::vector<std::string> completed_operations;
  bool activation_observed = false;
  std::uint32_t retries = 0;
  std::string last_error;
  Timestamp started_at;
  Timestamp updated_at;
  // Freshness of the last accepted verification evidence. Completion is
  // refused when this goes stale, even if the installer already exited.
  Timestamp evidence_at;
  Duration evidence_validity;
  Version observed_version;
  BuildId observed_build;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<AttemptRecord> from_json(const json::Value& value);
};

// A target's observed state, produced by an adapter observation or health probe.
struct [[nodiscard]] TargetObservation {
  TargetId target;
  Version version;
  BuildId build;
  bool healthy = false;
  // False when the target has no observable health at all (for example a
  // process that is not running yet). The gate is then recorded as not
  // applicable instead of silently passing or failing.
  bool health_known = true;
  std::string health_detail;
  Timestamp observed_at;
  Incarnation incarnation;
  std::string source;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<TargetObservation> from_json(const json::Value& value);
};

}  // namespace fum
