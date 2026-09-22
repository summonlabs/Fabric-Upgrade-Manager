#include "fum/model/campaign.hpp"

#include <algorithm>

#include "fum/core/hash.hpp"

namespace fum {
namespace {

Result<std::vector<std::string>> string_array(const json::Value& value, std::string_view key) {
  std::vector<std::string> out;
  const json::Value* found = value.find(key);
  if (found == nullptr || found->is_null()) {
    return out;
  }
  if (!found->is_array()) {
    return make_error(ErrorCode::invalid_argument, "json member must be an array",
                      std::string(key));
  }
  for (const auto& item : found->items()) {
    std::string_view text;
    FUM_TRY(text, item.as_string());
    out.emplace_back(text);
  }
  return out;
}

json::Value string_array_to_json(const std::vector<std::string>& values) {
  json::Value out = json::Value::make_array();
  for (const auto& value : values) {
    out.push(json::Value::make_string(value));
  }
  return out;
}

}  // namespace

json::Value PlanStep::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("step_id", json::Value::make_string(id.str()));
  value.set("action", json::Value::make_string(action));
  value.set("description", json::Value::make_string(description));
  value.set("irreversible", json::Value::make_bool(irreversible));
  value.set("irreversible_reason", json::Value::make_string(irreversible_reason));
  return value;
}

Result<PlanStep> PlanStep::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "plan step must be a json object");
  }
  PlanStep out;
  std::string text;
  FUM_TRY(text, value.require_string("step_id"));
  FUM_TRY(out.id, StepId::parse(text));
  FUM_TRY(out.action, value.require_string("action"));
  const json::Value* description = value.find("description");
  if (description != nullptr) {
    FUM_TRY(out.description, description->as_string());
  }
  const json::Value* irreversible = value.find("irreversible");
  if (irreversible != nullptr) {
    FUM_TRY(out.irreversible, irreversible->as_bool());
  }
  const json::Value* reason = value.find("irreversible_reason");
  if (reason != nullptr) {
    FUM_TRY(out.irreversible_reason, reason->as_string());
  }
  if (out.irreversible && out.irreversible_reason.empty()) {
    return make_error(ErrorCode::invalid_argument,
                      "an irreversible step must carry an explicit reason",
                      out.id.str());
  }
  return out;
}

json::Value StagePlan::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("stage_id", json::Value::make_string(id.str()));
  value.set("name", json::Value::make_string(name));
  json::Value targets_json = json::Value::make_array();
  for (const auto& target : targets) {
    targets_json.push(json::Value::make_string(target.str()));
  }
  value.set("targets", std::move(targets_json));
  json::Value steps_json = json::Value::make_array();
  for (const auto& step : steps) {
    steps_json.push(step.to_json());
  }
  value.set("steps", std::move(steps_json));
  value.set("requires_service_removal", json::Value::make_bool(requires_service_removal));
  value.set("redundancy_group", json::Value::make_string(redundancy_group));
  value.set("max_parallel", json::Value::make_uint(max_parallel));
  return value;
}

Result<StagePlan> StagePlan::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "stage plan must be a json object");
  }
  StagePlan out;
  std::string text;
  FUM_TRY(text, value.require_string("stage_id"));
  FUM_TRY(out.id, StageId::parse(text));
  const json::Value* name = value.find("name");
  if (name != nullptr) {
    FUM_TRY(out.name, name->as_string());
  }
  std::vector<std::string> target_texts;
  FUM_TRY(target_texts, string_array(value, "targets"));
  for (const auto& target_text : target_texts) {
    TargetId target;
    FUM_TRY(target, TargetId::parse(target_text));
    out.targets.push_back(std::move(target));
  }
  const json::Value* steps = value.find("steps");
  if (steps != nullptr && steps->is_array()) {
    for (const auto& item : steps->items()) {
      PlanStep step;
      FUM_TRY(step, PlanStep::from_json(item));
      out.steps.push_back(std::move(step));
    }
  }
  const json::Value* drain = value.find("requires_service_removal");
  if (drain != nullptr) {
    FUM_TRY(out.requires_service_removal, drain->as_bool());
  }
  const json::Value* group = value.find("redundancy_group");
  if (group != nullptr) {
    FUM_TRY(out.redundancy_group, group->as_string());
  }
  const json::Value* parallel = value.find("max_parallel");
  if (parallel != nullptr) {
    std::uint64_t parsed = 0;
    FUM_TRY(parsed, parallel->as_uint());
    out.max_parallel = static_cast<std::uint32_t>(parsed);
  }
  if (out.max_parallel == 0) {
    return make_error(ErrorCode::invalid_argument, "stage max_parallel must be at least 1",
                      out.id.str());
  }
  return out;
}

const char* plan_rejection_name(PlanRejectionCode code) noexcept {
  switch (code) {
    case PlanRejectionCode::no_targets: return "no-targets";
    case PlanRejectionCode::unknown_target: return "unknown-target";
    case PlanRejectionCode::mixed_component: return "mixed-component";
    case PlanRejectionCode::platform_unsupported: return "platform-unsupported";
    case PlanRejectionCode::capability_missing: return "capability-missing";
    case PlanRejectionCode::strategy_unsupported: return "strategy-unsupported";
    case PlanRejectionCode::duplicate_target: return "duplicate-target";
    case PlanRejectionCode::integrity_unverified: return "integrity-unverified";
    case PlanRejectionCode::version_regression: return "version-regression";
    case PlanRejectionCode::skew_unsatisfiable: return "skew-unsatisfiable";
    case PlanRejectionCode::irreversible_unacknowledged: return "irreversible-unacknowledged";
    case PlanRejectionCode::target_limit_exceeded: return "target-limit-exceeded";
    case PlanRejectionCode::stage_limit_exceeded: return "stage-limit-exceeded";
    case PlanRejectionCode::adapter_claim_conflict: return "adapter-claim-conflict";
    case PlanRejectionCode::artifact_component_mismatch: return "artifact-component-mismatch";
    case PlanRejectionCode::already_at_version: return "already-at-version";
  }
  return "unknown";
}

Result<PlanRejectionCode> parse_plan_rejection(std::string_view text) {
  for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(PlanRejectionCode::already_at_version);
       ++i) {
    const auto code = static_cast<PlanRejectionCode>(i);
    if (text == plan_rejection_name(code)) {
      return code;
    }
  }
  return make_error(ErrorCode::invalid_argument, "unknown plan rejection code", std::string(text));
}

json::Value PlanRejection::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("code", json::Value::make_string(plan_rejection_name(code)));
  value.set("subject", json::Value::make_string(subject));
  value.set("detail", json::Value::make_string(detail));
  return value;
}

Result<PlanRejection> PlanRejection::from_json(const json::Value& value) {
  PlanRejection out;
  std::string_view code_text;
  FUM_TRY(code_text, value.require_string("code"));
  FUM_TRY(out.code, parse_plan_rejection(code_text));
  FUM_TRY(out.subject, value.require_string("subject"));
  FUM_TRY(out.detail, value.require_string("detail"));
  return out;
}

json::Value UpgradePlanRequest::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("campaign_id", json::Value::make_string(campaign.str()));
  value.set("component_id", json::Value::make_string(component.str()));
  value.set("artifact", artifact.to_json());
  json::Value targets_json = json::Value::make_array();
  for (const auto& target : targets) {
    targets_json.push(json::Value::make_string(target.str()));
  }
  value.set("targets", std::move(targets_json));
  value.set("strategy", json::Value::make_string(strategy_kind_name(strategy)));
  value.set("skew_budget", skew_budget.to_json());
  value.set("authority", json::Value::make_string(authority.str()));
  value.set("acknowledge_irreversible_steps",
            json::Value::make_bool(acknowledge_irreversible_steps));
  json::Value acknowledged = json::Value::make_array();
  for (const auto& step : acknowledged_irreversible_steps) {
    acknowledged.push(json::Value::make_string(step.str()));
  }
  value.set("acknowledged_irreversible_steps", std::move(acknowledged));
  value.set("max_parallel_activations", json::Value::make_uint(max_parallel_activations));
  value.set("description", json::Value::make_string(description));
  value.set("requested_at", json::Value::make_string(requested_at.to_iso8601()));
  return value;
}

Result<UpgradePlanRequest> UpgradePlanRequest::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "plan request must be a json object");
  }
  UpgradePlanRequest out;
  std::string text;
  FUM_TRY(text, value.require_string("campaign_id"));
  FUM_TRY(out.campaign, CampaignId::parse(text));
  FUM_TRY(text, value.require_string("component_id"));
  FUM_TRY(out.component, ComponentId::parse(text));
  const json::Value* artifact = value.find("artifact");
  if (artifact == nullptr) {
    return make_error(ErrorCode::invalid_argument, "plan request requires an artifact");
  }
  FUM_TRY(out.artifact, ArtifactDescriptor::from_json(*artifact));
  std::vector<std::string> target_texts;
  FUM_TRY(target_texts, string_array(value, "targets"));
  for (const auto& target_text : target_texts) {
    TargetId target;
    FUM_TRY(target, TargetId::parse(target_text));
    out.targets.push_back(std::move(target));
  }
  const json::Value* strategy = value.find("strategy");
  if (strategy != nullptr) {
    std::string_view strategy_text;
    FUM_TRY(strategy_text, strategy->as_string());
    FUM_TRY(out.strategy, parse_strategy_kind(strategy_text));
  }
  const json::Value* budget = value.find("skew_budget");
  if (budget != nullptr) {
    FUM_TRY(out.skew_budget, SkewBudget::from_json(*budget));
  }
  FUM_TRY(text, value.require_string("authority"));
  FUM_TRY(out.authority, AuthorityId::parse(text));
  const json::Value* acknowledge = value.find("acknowledge_irreversible_steps");
  if (acknowledge != nullptr) {
    FUM_TRY(out.acknowledge_irreversible_steps, acknowledge->as_bool());
  }
  std::vector<std::string> step_texts;
  FUM_TRY(step_texts, string_array(value, "acknowledged_irreversible_steps"));
  for (const auto& step_text : step_texts) {
    StepId step;
    FUM_TRY(step, StepId::parse(step_text));
    out.acknowledged_irreversible_steps.push_back(std::move(step));
  }
  const json::Value* parallel = value.find("max_parallel_activations");
  if (parallel != nullptr) {
    std::uint64_t parsed = 0;
    FUM_TRY(parsed, parallel->as_uint());
    out.max_parallel_activations = static_cast<std::uint32_t>(parsed);
  }
  const json::Value* description = value.find("description");
  if (description != nullptr) {
    FUM_TRY(out.description, description->as_string());
  }
  const json::Value* requested = value.find("requested_at");
  if (requested != nullptr) {
    std::string_view requested_text;
    FUM_TRY(requested_text, requested->as_string());
    if (!requested_text.empty()) {
      FUM_TRY(out.requested_at, Timestamp::parse_iso8601(requested_text));
    }
  }
  return out;
}

std::size_t UpgradePlan::target_count() const {
  std::size_t count = 0;
  for (const auto& stage : stages) {
    count += stage.targets.size();
  }
  return count;
}

std::string UpgradePlan::fingerprint() const { return hash::sha256_hex(to_json().dump()); }

json::Value UpgradePlan::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("campaign_id", json::Value::make_string(campaign.str()));
  value.set("generation", json::Value::make_uint(generation.value()));
  value.set("authority", json::Value::make_string(authority.str()));
  value.set("component_id", json::Value::make_string(component.str()));
  value.set("artifact", artifact.to_json());
  value.set("strategy", json::Value::make_string(strategy_kind_name(strategy)));
  json::Value stages_json = json::Value::make_array();
  for (const auto& stage : stages) {
    stages_json.push(stage.to_json());
  }
  value.set("stages", std::move(stages_json));
  value.set("skew_budget", skew_budget.to_json());
  json::Value rejections_json = json::Value::make_array();
  for (const auto& rejection : rejections) {
    rejections_json.push(rejection.to_json());
  }
  value.set("rejections", std::move(rejections_json));
  json::Value irreversible = json::Value::make_array();
  for (const auto& step : irreversible_steps) {
    irreversible.push(json::Value::make_string(step.str()));
  }
  value.set("irreversible_steps", std::move(irreversible));
  value.set("policy", json::Value::make_string(policy.str()));
  value.set("policy_revision", json::Value::make_uint(policy_revision.value()));
  value.set("created_at", json::Value::make_string(created_at.to_iso8601()));
  value.set("description", json::Value::make_string(description));
  return value;
}

Result<UpgradePlan> UpgradePlan::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "upgrade plan must be a json object");
  }
  UpgradePlan out;
  std::string text;
  FUM_TRY(text, value.require_string("campaign_id"));
  FUM_TRY(out.campaign, CampaignId::parse(text));
  std::uint64_t numeric = 0;
  FUM_TRY(numeric, value.require_uint("generation"));
  out.generation = Generation(numeric);
  FUM_TRY(text, value.require_string("authority"));
  FUM_TRY(out.authority, AuthorityId::parse(text));
  FUM_TRY(text, value.require_string("component_id"));
  FUM_TRY(out.component, ComponentId::parse(text));
  const json::Value* artifact = value.find("artifact");
  if (artifact == nullptr) {
    return make_error(ErrorCode::invalid_argument, "upgrade plan requires an artifact");
  }
  FUM_TRY(out.artifact, ArtifactDescriptor::from_json(*artifact));
  std::string_view strategy_text;
  FUM_TRY(strategy_text, value.require_string("strategy"));
  FUM_TRY(out.strategy, parse_strategy_kind(strategy_text));
  const json::Value* stages = value.find("stages");
  if (stages != nullptr && stages->is_array()) {
    for (const auto& item : stages->items()) {
      StagePlan stage;
      FUM_TRY(stage, StagePlan::from_json(item));
      out.stages.push_back(std::move(stage));
    }
  }
  const json::Value* budget = value.find("skew_budget");
  if (budget != nullptr) {
    FUM_TRY(out.skew_budget, SkewBudget::from_json(*budget));
  }
  const json::Value* rejections = value.find("rejections");
  if (rejections != nullptr && rejections->is_array()) {
    for (const auto& item : rejections->items()) {
      PlanRejection rejection;
      FUM_TRY(rejection, PlanRejection::from_json(item));
      out.rejections.push_back(std::move(rejection));
    }
  }
  std::vector<std::string> irreversible_texts;
  FUM_TRY(irreversible_texts, string_array(value, "irreversible_steps"));
  for (const auto& step_text : irreversible_texts) {
    StepId step;
    FUM_TRY(step, StepId::parse(step_text));
    out.irreversible_steps.push_back(std::move(step));
  }
  FUM_TRY(text, value.require_string("policy"));
  FUM_TRY(out.policy, PolicyId::parse(text));
  FUM_TRY(numeric, value.require_uint("policy_revision"));
  out.policy_revision = Revision(numeric);
  const json::Value* created = value.find("created_at");
  if (created != nullptr) {
    std::string_view created_text;
    FUM_TRY(created_text, created->as_string());
    FUM_TRY(out.created_at, Timestamp::parse_iso8601(created_text));
  }
  const json::Value* description = value.find("description");
  if (description != nullptr) {
    FUM_TRY(out.description, description->as_string());
  }
  return out;
}

json::Value StageProgress::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("stage_id", json::Value::make_string(id.str()));
  value.set("state", json::Value::make_string(stage_state_name(state)));
  json::Value attempts_json = json::Value::make_array();
  for (const auto& attempt : attempts) {
    attempts_json.push(json::Value::make_string(attempt.str()));
  }
  value.set("attempts", std::move(attempts_json));
  value.set("started_at", json::Value::make_string(started_at.to_iso8601()));
  value.set("completed_at", json::Value::make_string(completed_at.to_iso8601()));
  value.set("detail", json::Value::make_string(detail));
  return value;
}

Result<StageProgress> StageProgress::from_json(const json::Value& value) {
  StageProgress out;
  std::string text;
  FUM_TRY(text, value.require_string("stage_id"));
  FUM_TRY(out.id, StageId::parse(text));
  std::string_view state_text;
  FUM_TRY(state_text, value.require_string("state"));
  bool matched = false;
  for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(StageState::skipped); ++i) {
    const auto state = static_cast<StageState>(i);
    if (state_text == stage_state_name(state)) {
      out.state = state;
      matched = true;
      break;
    }
  }
  if (!matched) {
    return make_error(ErrorCode::invalid_argument, "unknown stage state", std::string(state_text));
  }
  std::vector<std::string> attempt_texts;
  FUM_TRY(attempt_texts, string_array(value, "attempts"));
  for (const auto& attempt_text : attempt_texts) {
    AttemptId attempt;
    FUM_TRY(attempt, AttemptId::parse(attempt_text));
    out.attempts.push_back(std::move(attempt));
  }
  const json::Value* started = value.find("started_at");
  if (started != nullptr) {
    std::string_view started_text;
    FUM_TRY(started_text, started->as_string());
    FUM_TRY(out.started_at, Timestamp::parse_iso8601(started_text));
  }
  const json::Value* completed = value.find("completed_at");
  if (completed != nullptr) {
    std::string_view completed_text;
    FUM_TRY(completed_text, completed->as_string());
    FUM_TRY(out.completed_at, Timestamp::parse_iso8601(completed_text));
  }
  const json::Value* detail = value.find("detail");
  if (detail != nullptr) {
    FUM_TRY(out.detail, detail->as_string());
  }
  return out;
}

json::Value PreflightTargetVerdict::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("target_id", json::Value::make_string(target.str()));
  value.set("compatible", json::Value::make_bool(compatible));
  value.set("integrity_verified", json::Value::make_bool(integrity_verified));
  value.set("platform_supported", json::Value::make_bool(platform_supported));
  value.set("strategy_supported", json::Value::make_bool(strategy_supported));
  value.set("capabilities_satisfied", json::Value::make_bool(capabilities_satisfied));
  value.set("rollback_available", json::Value::make_bool(rollback_available));
  value.set("reasons", string_array_to_json(reasons));
  return value;
}

Result<PreflightTargetVerdict> PreflightTargetVerdict::from_json(const json::Value& value) {
  PreflightTargetVerdict out;
  std::string text;
  FUM_TRY(text, value.require_string("target_id"));
  FUM_TRY(out.target, TargetId::parse(text));
  const json::Value* flags[] = {value.find("compatible"), value.find("integrity_verified"),
                                value.find("platform_supported"),
                                value.find("strategy_supported"),
                                value.find("capabilities_satisfied"),
                                value.find("rollback_available")};
  bool* targets[] = {&out.compatible,        &out.integrity_verified, &out.platform_supported,
                     &out.strategy_supported, &out.capabilities_satisfied, &out.rollback_available};
  for (std::size_t i = 0; i < 6; ++i) {
    if (flags[i] != nullptr) {
      FUM_TRY(*targets[i], flags[i]->as_bool());
    }
  }
  FUM_TRY(out.reasons, string_array(value, "reasons"));
  return out;
}

json::Value PreflightTicket::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("campaign_id", json::Value::make_string(campaign.str()));
  value.set("generation", json::Value::make_uint(generation.value()));
  value.set("incarnation", json::Value::make_uint(incarnation.value()));
  value.set("epoch", json::Value::make_uint(epoch.value()));
  value.set("issued_at", json::Value::make_string(issued_at.to_iso8601()));
  value.set("expires_at", json::Value::make_string(expires_at.to_iso8601()));
  value.set("fingerprint", json::Value::make_string(fingerprint));
  value.set("passed", json::Value::make_bool(passed));
  value.set("authority", json::Value::make_string(authority.str()));
  value.set("policy", json::Value::make_string(policy.str()));
  value.set("policy_revision", json::Value::make_uint(policy_revision.value()));
  return value;
}

Result<PreflightTicket> PreflightTicket::from_json(const json::Value& value) {
  PreflightTicket out;
  std::string text;
  FUM_TRY(text, value.require_string("campaign_id"));
  FUM_TRY(out.campaign, CampaignId::parse(text));
  std::uint64_t numeric = 0;
  FUM_TRY(numeric, value.require_uint("generation"));
  out.generation = Generation(numeric);
  FUM_TRY(numeric, value.require_uint("incarnation"));
  out.incarnation = Incarnation(numeric);
  FUM_TRY(numeric, value.require_uint("epoch"));
  out.epoch = Epoch(numeric);
  std::string_view issued_text;
  FUM_TRY(issued_text, value.require_string("issued_at"));
  FUM_TRY(out.issued_at, Timestamp::parse_iso8601(issued_text));
  std::string_view expires_text;
  FUM_TRY(expires_text, value.require_string("expires_at"));
  FUM_TRY(out.expires_at, Timestamp::parse_iso8601(expires_text));
  FUM_TRY(out.fingerprint, value.require_string("fingerprint"));
  FUM_TRY(out.passed, value.require_bool("passed"));
  FUM_TRY(text, value.require_string("authority"));
  FUM_TRY(out.authority, AuthorityId::parse(text));
  FUM_TRY(text, value.require_string("policy"));
  FUM_TRY(out.policy, PolicyId::parse(text));
  FUM_TRY(numeric, value.require_uint("policy_revision"));
  out.policy_revision = Revision(numeric);
  return out;
}

json::Value CampaignRecord::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("campaign_id", json::Value::make_string(id.str()));
  value.set("generation", json::Value::make_uint(generation.value()));
  value.set("epoch", json::Value::make_uint(epoch.value()));
  value.set("authority", json::Value::make_string(authority.str()));
  value.set("component_id", json::Value::make_string(component.str()));
  value.set("state", json::Value::make_string(upgrade_state_name(state)));
  value.set("strategy", json::Value::make_string(strategy_kind_name(strategy)));
  if (!artifact.id().empty()) {
    value.set("artifact", artifact.to_json());
  }
  if (!plan.campaign.empty()) {
    value.set("plan", plan.to_json());
  }
  json::Value stages_json = json::Value::make_array();
  for (const auto& stage : stages) {
    stages_json.push(stage.to_json());
  }
  value.set("stages", std::move(stages_json));
  value.set("active_stage", json::Value::make_uint(static_cast<std::uint64_t>(active_stage)));
  value.set("revision", json::Value::make_uint(revision.value()));
  value.set("owner_incarnation", json::Value::make_uint(owner_incarnation.value()));
  value.set("created_at", json::Value::make_string(created_at.to_iso8601()));
  value.set("updated_at", json::Value::make_string(updated_at.to_iso8601()));
  value.set("irreversible_acknowledged", json::Value::make_bool(irreversible_acknowledged));
  json::Value acknowledged = json::Value::make_array();
  for (const auto& step : acknowledged_steps) {
    acknowledged.push(json::Value::make_string(step.str()));
  }
  value.set("acknowledged_steps", std::move(acknowledged));
  value.set("block_reason", json::Value::make_string(block_reason));
  value.set("failure_reason", json::Value::make_string(failure_reason));
  value.set("pause_reason", json::Value::make_string(pause_reason));
  value.set("rollback_plan", json::Value::make_string(rollback_plan));
  value.set("rollback_artifact", json::Value::make_string(rollback_artifact));
  value.set("rollback_eligible", json::Value::make_bool(rollback_eligible));
  value.set("rollback_performed", json::Value::make_bool(rollback_performed));
  value.set("attempts_started", json::Value::make_uint(attempts_started));
  value.set("decisions_recorded", json::Value::make_uint(decisions_recorded));
  if (ticket.issued_at.unix_nanos() != 0) {
    value.set("ticket", ticket.to_json());
  }
  // last_skew is deliberately not persisted: it is derived from live attempts
  // and inventory, and a deserialized assessment must never be treated as
  // current evidence.
  return value;
}

Result<CampaignRecord> CampaignRecord::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "campaign record must be a json object");
  }
  CampaignRecord out;
  std::string text;
  FUM_TRY(text, value.require_string("campaign_id"));
  FUM_TRY(out.id, CampaignId::parse(text));
  std::uint64_t numeric = 0;
  FUM_TRY(numeric, value.require_uint("generation"));
  out.generation = Generation(numeric);
  FUM_TRY(numeric, value.require_uint("epoch"));
  out.epoch = Epoch(numeric);
  FUM_TRY(text, value.require_string("authority"));
  FUM_TRY(out.authority, AuthorityId::parse(text));
  FUM_TRY(text, value.require_string("component_id"));
  FUM_TRY(out.component, ComponentId::parse(text));
  std::string_view state_text;
  FUM_TRY(state_text, value.require_string("state"));
  FUM_TRY(out.state, parse_upgrade_state(state_text));
  std::string_view strategy_text;
  FUM_TRY(strategy_text, value.require_string("strategy"));
  FUM_TRY(out.strategy, parse_strategy_kind(strategy_text));
  const json::Value* artifact = value.find("artifact");
  if (artifact != nullptr) {
    FUM_TRY(out.artifact, ArtifactDescriptor::from_json(*artifact));
  }
  const json::Value* plan = value.find("plan");
  if (plan != nullptr) {
    FUM_TRY(out.plan, UpgradePlan::from_json(*plan));
  }
  const json::Value* stages = value.find("stages");
  if (stages != nullptr && stages->is_array()) {
    for (const auto& item : stages->items()) {
      StageProgress stage;
      FUM_TRY(stage, StageProgress::from_json(item));
      out.stages.push_back(std::move(stage));
    }
  }
  const json::Value* active = value.find("active_stage");
  if (active != nullptr) {
    FUM_TRY(numeric, active->as_uint());
    out.active_stage = static_cast<std::size_t>(numeric);
  }
  FUM_TRY(numeric, value.require_uint("revision"));
  out.revision = Revision(numeric);
  FUM_TRY(numeric, value.require_uint("owner_incarnation"));
  out.owner_incarnation = Incarnation(numeric);
  const json::Value* created = value.find("created_at");
  if (created != nullptr) {
    std::string_view created_text;
    FUM_TRY(created_text, created->as_string());
    FUM_TRY(out.created_at, Timestamp::parse_iso8601(created_text));
  }
  const json::Value* updated = value.find("updated_at");
  if (updated != nullptr) {
    std::string_view updated_text;
    FUM_TRY(updated_text, updated->as_string());
    FUM_TRY(out.updated_at, Timestamp::parse_iso8601(updated_text));
  }
  const json::Value* acknowledged = value.find("irreversible_acknowledged");
  if (acknowledged != nullptr) {
    FUM_TRY(out.irreversible_acknowledged, acknowledged->as_bool());
  }
  std::vector<std::string> step_texts;
  FUM_TRY(step_texts, string_array(value, "acknowledged_steps"));
  for (const auto& step_text : step_texts) {
    StepId step;
    FUM_TRY(step, StepId::parse(step_text));
    out.acknowledged_steps.push_back(std::move(step));
  }
  const char* const string_members[] = {"block_reason", "failure_reason", "pause_reason",
                                        "rollback_plan", "rollback_artifact"};
  std::string* targets[] = {&out.block_reason, &out.failure_reason, &out.pause_reason,
                            &out.rollback_plan, &out.rollback_artifact};
  for (std::size_t i = 0; i < 5; ++i) {
    const json::Value* item = value.find(string_members[i]);
    if (item != nullptr) {
      FUM_TRY(*targets[i], item->as_string());
    }
  }
  const json::Value* eligible = value.find("rollback_eligible");
  if (eligible != nullptr) {
    FUM_TRY(out.rollback_eligible, eligible->as_bool());
  }
  const json::Value* performed = value.find("rollback_performed");
  if (performed != nullptr) {
    FUM_TRY(out.rollback_performed, performed->as_bool());
  }
  const json::Value* attempts = value.find("attempts_started");
  if (attempts != nullptr) {
    FUM_TRY(numeric, attempts->as_uint());
    out.attempts_started = static_cast<std::uint32_t>(numeric);
  }
  const json::Value* decisions = value.find("decisions_recorded");
  if (decisions != nullptr) {
    FUM_TRY(numeric, decisions->as_uint());
    out.decisions_recorded = static_cast<std::uint32_t>(numeric);
  }
  const json::Value* ticket = value.find("ticket");
  if (ticket != nullptr) {
    FUM_TRY(out.ticket, PreflightTicket::from_json(*ticket));
  }
  // Derived evidence (skew, freshness) is recomputed from live observations and
  // is never restored from durable state.
  out.last_skew.within_budget = false;
  out.last_skew.summary = "not assessed since this record was loaded";
  return out;
}

json::Value AttemptRecord::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("attempt_id", json::Value::make_string(id.str()));
  value.set("campaign_id", json::Value::make_string(campaign.str()));
  value.set("generation", json::Value::make_uint(generation.value()));
  value.set("stage_id", json::Value::make_string(stage.str()));
  value.set("target_id", json::Value::make_string(target.str()));
  value.set("artifact_id", json::Value::make_string(artifact.str()));
  value.set("build_id", json::Value::make_string(build.str()));
  value.set("from_version", json::Value::make_string(from_version.text()));
  value.set("to_version", json::Value::make_string(to_version.text()));
  value.set("state", json::Value::make_string(attempt_state_name(state)));
  value.set("owner_incarnation", json::Value::make_uint(owner.value()));
  value.set("authority", json::Value::make_string(authority.str()));
  value.set("revision", json::Value::make_uint(revision.value()));
  value.set("operation_index", json::Value::make_uint(operation_index.value()));
  value.set("completed_operations", string_array_to_json(completed_operations));
  value.set("activation_observed", json::Value::make_bool(activation_observed));
  value.set("retries", json::Value::make_uint(retries));
  value.set("last_error", json::Value::make_string(last_error));
  value.set("started_at", json::Value::make_string(started_at.to_iso8601()));
  value.set("updated_at", json::Value::make_string(updated_at.to_iso8601()));
  value.set("evidence_at", json::Value::make_string(evidence_at.to_iso8601()));
  value.set("evidence_validity_ms", json::Value::make_int(evidence_validity.nanos() / 1000000));
  value.set("observed_version", json::Value::make_string(observed_version.text()));
  value.set("observed_build", json::Value::make_string(observed_build.str()));
  return value;
}

Result<AttemptRecord> AttemptRecord::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "attempt record must be a json object");
  }
  AttemptRecord out;
  std::string text;
  FUM_TRY(text, value.require_string("attempt_id"));
  FUM_TRY(out.id, AttemptId::parse(text));
  FUM_TRY(text, value.require_string("campaign_id"));
  FUM_TRY(out.campaign, CampaignId::parse(text));
  std::uint64_t numeric = 0;
  FUM_TRY(numeric, value.require_uint("generation"));
  out.generation = Generation(numeric);
  FUM_TRY(text, value.require_string("stage_id"));
  FUM_TRY(out.stage, StageId::parse(text));
  FUM_TRY(text, value.require_string("target_id"));
  FUM_TRY(out.target, TargetId::parse(text));
  FUM_TRY(text, value.require_string("artifact_id"));
  FUM_TRY(out.artifact, ArtifactId::parse(text));
  FUM_TRY(text, value.require_string("build_id"));
  FUM_TRY(out.build, BuildId::parse(text));
  std::string_view version_text;
  FUM_TRY(version_text, value.require_string("from_version"));
  FUM_TRY(out.from_version, Version::parse(version_text));
  FUM_TRY(version_text, value.require_string("to_version"));
  FUM_TRY(out.to_version, Version::parse(version_text));
  std::string_view state_text;
  FUM_TRY(state_text, value.require_string("state"));
  bool matched = false;
  for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(AttemptState::abandoned); ++i) {
    const auto state = static_cast<AttemptState>(i);
    if (state_text == attempt_state_name(state)) {
      out.state = state;
      matched = true;
      break;
    }
  }
  if (!matched) {
    return make_error(ErrorCode::invalid_argument, "unknown attempt state", std::string(state_text));
  }
  FUM_TRY(numeric, value.require_uint("owner_incarnation"));
  out.owner = Incarnation(numeric);
  FUM_TRY(text, value.require_string("authority"));
  FUM_TRY(out.authority, AuthorityId::parse(text));
  FUM_TRY(numeric, value.require_uint("revision"));
  out.revision = Revision(numeric);
  FUM_TRY(numeric, value.require_uint("operation_index"));
  out.operation_index = Sequence(numeric);
  FUM_TRY(out.completed_operations, string_array(value, "completed_operations"));
  const json::Value* observed = value.find("activation_observed");
  if (observed != nullptr) {
    FUM_TRY(out.activation_observed, observed->as_bool());
  }
  const json::Value* retries = value.find("retries");
  if (retries != nullptr) {
    FUM_TRY(numeric, retries->as_uint());
    out.retries = static_cast<std::uint32_t>(numeric);
  }
  const json::Value* error = value.find("last_error");
  if (error != nullptr) {
    FUM_TRY(out.last_error, error->as_string());
  }
  const json::Value* started = value.find("started_at");
  if (started != nullptr) {
    std::string_view started_text;
    FUM_TRY(started_text, started->as_string());
    FUM_TRY(out.started_at, Timestamp::parse_iso8601(started_text));
  }
  const json::Value* updated = value.find("updated_at");
  if (updated != nullptr) {
    std::string_view updated_text;
    FUM_TRY(updated_text, updated->as_string());
    FUM_TRY(out.updated_at, Timestamp::parse_iso8601(updated_text));
  }
  const json::Value* evidence = value.find("evidence_at");
  if (evidence != nullptr) {
    std::string_view evidence_text;
    FUM_TRY(evidence_text, evidence->as_string());
    if (!evidence_text.empty()) {
      FUM_TRY(out.evidence_at, Timestamp::parse_iso8601(evidence_text));
    }
  }
  const json::Value* validity = value.find("evidence_validity_ms");
  if (validity != nullptr) {
    std::int64_t millis = 0;
    FUM_TRY(millis, validity->as_int());
    if (millis > 0) {
      out.evidence_validity = Duration::from_millis(millis);
    }
  }
  const json::Value* observed_version_json = value.find("observed_version");
  if (observed_version_json != nullptr) {
    std::string_view observed_version_text;
    FUM_TRY(observed_version_text, observed_version_json->as_string());
    if (!observed_version_text.empty()) {
      FUM_TRY(out.observed_version, Version::parse(observed_version_text));
    }
  }
  const json::Value* observed_build = value.find("observed_build");
  if (observed_build != nullptr) {
    std::string_view build_text;
    FUM_TRY(build_text, observed_build->as_string());
    if (!build_text.empty()) {
      FUM_TRY(out.observed_build, BuildId::parse(build_text));
    }
  }
  return out;
}

json::Value TargetObservation::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("target_id", json::Value::make_string(target.str()));
  value.set("version", json::Value::make_string(version.text()));
  value.set("build_id", json::Value::make_string(build.str()));
  value.set("healthy", json::Value::make_bool(healthy));
  value.set("health_known", json::Value::make_bool(health_known));
  value.set("health_detail", json::Value::make_string(health_detail));
  value.set("observed_at", json::Value::make_string(observed_at.to_iso8601()));
  value.set("incarnation", json::Value::make_uint(incarnation.value()));
  value.set("source", json::Value::make_string(source));
  return value;
}

Result<TargetObservation> TargetObservation::from_json(const json::Value& value) {
  TargetObservation out;
  std::string text;
  FUM_TRY(text, value.require_string("target_id"));
  FUM_TRY(out.target, TargetId::parse(text));
  std::string_view version_text;
  FUM_TRY(version_text, value.require_string("version"));
  FUM_TRY(out.version, Version::parse(version_text));
  FUM_TRY(text, value.require_string("build_id"));
  if (!text.empty()) {
    FUM_TRY(out.build, BuildId::parse(text));
  }
  FUM_TRY(out.healthy, value.require_bool("healthy"));
  if (const json::Value* known = value.find("health_known"); known != nullptr) {
    FUM_TRY(out.health_known, known->as_bool());
  }
  const json::Value* detail = value.find("health_detail");
  if (detail != nullptr) {
    FUM_TRY(out.health_detail, detail->as_string());
  }
  std::string_view observed_text;
  FUM_TRY(observed_text, value.require_string("observed_at"));
  FUM_TRY(out.observed_at, Timestamp::parse_iso8601(observed_text));
  std::uint64_t numeric = 0;
  FUM_TRY(numeric, value.require_uint("incarnation"));
  out.incarnation = Incarnation(numeric);
  const json::Value* source = value.find("source");
  if (source != nullptr) {
    FUM_TRY(out.source, source->as_string());
  }
  return out;
}

}  // namespace fum
