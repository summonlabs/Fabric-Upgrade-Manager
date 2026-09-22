// Engine: operator commands, admission and inspection.
#include <algorithm>
#include <utility>

#include "fum/engine/engine.hpp"

namespace fum {
namespace {

std::string step_list(const std::vector<StepId>& steps) {
  std::string out;
  for (const auto& step : steps) {
    if (!out.empty()) {
      out.append(", ");
    }
    out.append(step.str());
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Admission
// ---------------------------------------------------------------------------
Status Engine::start(const CampaignId& campaign_id, const StartOptions& options) {
  if (shutting_down_.load()) {
    return make_error(ErrorCode::cancelled, "the engine has been shut down");
  }
  const Timestamp now = deps_.clock->now();
  std::string rollout_change;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const CampaignRecord* stored = store_.campaign(campaign_id);
    if (stored == nullptr) {
      return make_error(ErrorCode::not_found, "unknown campaign", campaign_id.str());
    }
    CampaignRecord campaign = *stored;
    if (upgrade_state_is_terminal(campaign.state)) {
      return make_error(ErrorCode::precondition_failed, "the campaign is already terminal",
                        upgrade_state_name(campaign.state));
    }
    if (upgrade_state_is_active(campaign.state)) {
      return make_error(ErrorCode::precondition_failed, "the campaign is already executing",
                        upgrade_state_name(campaign.state));
    }
    if (!campaign.plan.executable()) {
      return make_error(ErrorCode::precondition_failed,
                        "the plan is rejected and cannot be executed",
                        campaign.plan.rejections.empty()
                            ? std::string("unspecified rejection")
                            : std::string(plan_rejection_name(campaign.plan.rejections.front().code)) +
                                  ": " + campaign.plan.rejections.front().detail);
    }
    std::string ticket_detail;
    if (!ticket_current_locked(campaign, now, ticket_detail)) {
      return make_error(ErrorCode::stale,
                        "preflight is not current; run preflight before starting", ticket_detail);
    }

    // Rollback eligibility and the recorded rollback plan are prerequisites of
    // execution, not afterthoughts.
    bool adapters_support_rollback = true;
    for (const auto& stage : campaign.plan.stages) {
      for (const auto& target_id : stage.targets) {
        const TargetDescriptor* descriptor = last_inventory_.find(target_id);
        if (descriptor == nullptr || !descriptor->claims.supports_rollback) {
          adapters_support_rollback = false;
        }
      }
    }
    const bool reversible = campaign.artifact.rollback().reversible;
    campaign.rollback_eligible = reversible && adapters_support_rollback;
    campaign.rollback_artifact = campaign.artifact.rollback().rollback_artifact.str();
    if (!reversible) {
      campaign.rollback_plan =
          std::string("no rollback: the artifact declares the boundary '") +
          (campaign.artifact.rollback().boundary.empty() ? "unspecified"
                                                         : campaign.artifact.rollback().boundary) +
          "' and cannot be reversed";
    } else if (!adapters_support_rollback) {
      campaign.rollback_plan =
          "no rollback: at least one target adapter does not claim rollback support";
    } else if (!campaign.rollback_artifact.empty()) {
      campaign.rollback_plan = "restore artifact " + campaign.rollback_artifact +
                               " on every activated target through the adapter rollback path";
    } else {
      campaign.rollback_plan =
          "re-activate the previously installed build on every activated target through the "
          "adapter rollback path";
    }

    if (options.allow_irreversible && !campaign.irreversible_acknowledged) {
      campaign.irreversible_acknowledged = true;
      campaign.acknowledged_steps = campaign.plan.irreversible_steps;
    }
    TransitionGuards guards = guards_locked(campaign);
    guards.rollback_plan_recorded = true;
    guards.irreversible_acknowledged =
        campaign.plan.irreversible_steps.empty() || campaign.irreversible_acknowledged;
    if (!guards.irreversible_acknowledged) {
      return make_error(ErrorCode::irreversible,
                        "the plan contains irreversible steps that the operator has not "
                        "acknowledged",
                        step_list(campaign.plan.irreversible_steps));
    }
    if (campaign.state == UpgradeState::proposed) {
      return make_error(ErrorCode::precondition_failed,
                        "a campaign must pass preflight before it can start",
                        upgrade_state_name(campaign.state));
    }

    const Generation previous_generation = campaign.generation;
    if (auto next = campaign.generation.try_next(); next.has_value()) {
      campaign.generation = next.value();
    }
    campaign.owner_incarnation = incarnation_;
    // The ticket is re-bound to the new execution generation. The preflight
    // evidence itself is unchanged and must still be inside its validity window.
    campaign.ticket.generation = campaign.generation;
    campaign.updated_at = now;
    if (auto revision = campaign.revision.try_next(); revision.has_value()) {
      campaign.revision = revision.value();
    }
    FUM_TRYV(store_.put_campaign(campaign, now));
    FUM_TRYV(store_.put_ticket(campaign.ticket, now));
    {
      const Decision decision = make_decision_locked(
          DecisionKind::admit, campaign, DecisionOutcome::allow, "admit the campaign for execution",
          "preflight is current and the rollback plan is recorded",
          {DecisionInput{"previous_generation", std::to_string(previous_generation.value())},
           DecisionInput{"execution_generation", std::to_string(campaign.generation.value())},
           DecisionInput{"rollback_eligible", campaign.rollback_eligible ? "true" : "false"},
           DecisionInput{"rollback_plan", campaign.rollback_plan},
           DecisionInput{"ticket_fingerprint", campaign.ticket.fingerprint}},
          {RejectedAlternative{"execute without a rollback plan",
                               "rollback readiness must be known before the first activation"}});
      record_decision_locked(decision);
    }

    if (campaign.state == UpgradeState::paused || campaign.state == UpgradeState::blocked) {
      campaign.pause_reason.clear();
      campaign.block_reason.clear();
      guards.manual_release_required = true;
      FUM_TRYV(transition_locked(campaign, UpgradeState::validated, guards));
    }
    if (campaign.state == UpgradeState::validated) {
      FUM_TRYV(transition_locked(campaign, UpgradeState::prepared, guards));
    }
    const auto change = rollout_changes_.find(campaign.id.str());
    if (change != rollout_changes_.end()) {
      rollout_change = change->second;
    }
  }

  if (deps_.rollout && rollout_change.empty() && !options.dry_run) {
    RolloutRequest request;
    request.campaign = campaign_id;
    request.authority = make_AuthorityId("");
    request.description = options.reason;
    request.incarnation = incarnation_;
    request.at = now;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const CampaignRecord* stored = store_.campaign(campaign_id);
      if (stored != nullptr) {
        request.authority = stored->authority;
        request.component = stored->component;
        request.description = stored->plan.description;
        request.target_count = stored->plan.target_count();
      }
    }
    auto admission = deps_.rollout->admit(request);
    if (!admission.has_value()) {
      return admission.error();
    }
    if (!admission.value().admitted) {
      return make_error(ErrorCode::policy_denied,
                        "the Change Planner/Rollout Fabric refused admission",
                        admission.value().detail);
    }
    rollout_change = admission.value().change_id;
    const std::lock_guard<std::mutex> lock(mutex_);
    rollout_changes_[campaign_id.str()] = rollout_change;
  }

  if (options.dry_run) {
    return ok_status();
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    admissions_.fetch_add(1);
    if (!pool_mode_) {
      return ok_status();   // the caller drives with run_steps/run_until_idle
    }
    if (driver_active_) {
      // The running driver re-checks the admission counter before going idle,
      // so this campaign is picked up without a second driver.
      return ok_status();
    }
    driver_active_ = true;
  }
  const Status submitted = pool_->submit([this] { driver_loop(); });
  if (!submitted.has_value()) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      driver_active_ = false;
    }
    idle_cv_.notify_all();
    return submitted.error();
  }
  return ok_status();
}

Status Engine::pause(const CampaignId& campaign_id, std::string reason) {
  if (shutting_down_.load()) {
    return make_error(ErrorCode::cancelled, "the engine has been shut down");
  }
  const Timestamp now = deps_.clock->now();
  const std::lock_guard<std::mutex> lock(mutex_);
  const CampaignRecord* stored = store_.campaign(campaign_id);
  if (stored == nullptr) {
    return make_error(ErrorCode::not_found, "unknown campaign", campaign_id.str());
  }
  CampaignRecord campaign = *stored;
  if (campaign.state == UpgradeState::paused) {
    return ok_status();
  }
  if (!upgrade_state_permits_pause(campaign.state)) {
    return make_error(
        ErrorCode::precondition_failed,
        "a campaign can only be paused at a stage boundary; the current state does not allow it",
        upgrade_state_name(campaign.state));
  }
  campaign.pause_reason = reason.empty() ? "paused by operator" : std::move(reason);
  TransitionGuards guards = guards_locked(campaign);
  guards.manual_release_required = true;
  FUM_TRYV(transition_locked(campaign, UpgradeState::paused, guards));

  // In-flight operations cannot be allowed to commit into a paused campaign:
  // they are retired explicitly so the operator sees an honest state.
  for (const auto& attempt : store_.attempts_for(campaign_id)) {
    if (!attempt_state_is_in_flight(attempt.state)) {
      continue;
    }
    AttemptRecord updated = attempt;
    updated.state = AttemptState::abandoned;
    updated.last_error = "the campaign was paused while this operation was in flight";
    updated.updated_at = now;
    if (auto revision = updated.revision.try_next(); revision.has_value()) {
      updated.revision = revision.value();
    }
    FUM_TRYV(store_.put_attempt(updated, now));
  }
  const Decision decision = make_decision_locked(
      DecisionKind::pause, campaign, DecisionOutcome::allow, "pause at a stage boundary",
      campaign.pause_reason,
      {DecisionInput{"state", upgrade_state_name(campaign.state)},
       DecisionInput{"active_stage", std::to_string(campaign.active_stage + 1)}});
  record_decision_locked(decision);
  return ok_status();
}

Status Engine::resume(const CampaignId& campaign_id) {
  if (shutting_down_.load()) {
    return make_error(ErrorCode::cancelled, "the engine has been shut down");
  }
  const Timestamp now = deps_.clock->now();
  const std::lock_guard<std::mutex> lock(mutex_);
  const CampaignRecord* stored = store_.campaign(campaign_id);
  if (stored == nullptr) {
    return make_error(ErrorCode::not_found, "unknown campaign", campaign_id.str());
  }
  CampaignRecord campaign = *stored;
  if (campaign.state == UpgradeState::staged || campaign.state == UpgradeState::activating ||
      campaign.state == UpgradeState::verifying || campaign.state == UpgradeState::rolling_back) {
    return ok_status();
  }
  if (campaign.state != UpgradeState::paused && campaign.state != UpgradeState::blocked &&
      campaign.state != UpgradeState::validated && campaign.state != UpgradeState::prepared) {
    return make_error(ErrorCode::precondition_failed,
                      "only a paused or blocked campaign can be resumed",
                      upgrade_state_name(campaign.state));
  }
  std::string detail;
  if (!ticket_current_locked(campaign, now, detail)) {
    return make_error(ErrorCode::stale,
                      "a fresh preflight is required before this campaign can be resumed", detail);
  }
  TransitionGuards guards = guards_locked(campaign);
  guards.manual_release_required = true;
  campaign.pause_reason.clear();
  campaign.block_reason.clear();
  if (campaign.state == UpgradeState::paused || campaign.state == UpgradeState::blocked) {
    // A preflight pass may already have moved a blocked campaign to validated;
    // resume continues from wherever the campaign actually is.
    FUM_TRYV(transition_locked(campaign, UpgradeState::validated, guards));
  }
  guards.rollback_plan_recorded = !campaign.rollback_plan.empty();
  guards.irreversible_acknowledged =
      campaign.plan.irreversible_steps.empty() || campaign.irreversible_acknowledged;
  FUM_TRYV(transition_locked(campaign, UpgradeState::prepared, guards));
  const Decision decision = make_decision_locked(
      DecisionKind::resume, campaign, DecisionOutcome::allow, "resume execution",
      "the operator released the campaign and preflight is still current",
      {DecisionInput{"active_stage", std::to_string(campaign.active_stage + 1)}});
  record_decision_locked(decision);
  return ok_status();
}

Status Engine::abort(const CampaignId& campaign_id, std::string reason) {
  if (shutting_down_.load()) {
    return make_error(ErrorCode::cancelled, "the engine has been shut down");
  }
  const Timestamp now = deps_.clock->now();
  const std::lock_guard<std::mutex> lock(mutex_);
  const CampaignRecord* stored = store_.campaign(campaign_id);
  if (stored == nullptr) {
    return make_error(ErrorCode::not_found, "unknown campaign", campaign_id.str());
  }
  CampaignRecord campaign = *stored;
  if (upgrade_state_is_terminal(campaign.state)) {
    return make_error(ErrorCode::precondition_failed, "the campaign is already terminal",
                      upgrade_state_name(campaign.state));
  }
  for (const auto& attempt : store_.attempts_for(campaign_id)) {
    if (!attempt_state_is_in_flight(attempt.state)) {
      continue;
    }
    AttemptRecord updated = attempt;
    updated.state = AttemptState::aborted;
    updated.last_error = "the campaign was aborted while this operation was in flight";
    updated.updated_at = now;
    if (auto revision = updated.revision.try_next(); revision.has_value()) {
      updated.revision = revision.value();
    }
    FUM_TRYV(store_.put_attempt(updated, now));
  }
  campaign.failure_reason = reason.empty() ? "aborted by operator" : std::move(reason);
  TransitionGuards guards = guards_locked(campaign);
  guards.failure_recorded = true;
  const UpgradeState target =
      campaign.state == UpgradeState::proposed ? UpgradeState::blocked : UpgradeState::failed;
  FUM_TRYV(transition_locked(campaign, target, guards));
  const Decision decision = make_decision_locked(
      DecisionKind::abort, campaign, DecisionOutcome::allow, "abort execution",
      campaign.failure_reason,
      {DecisionInput{"state", upgrade_state_name(campaign.state)} });
  record_decision_locked(decision);
  return ok_status();
}

Result<PreflightReport> Engine::rollback(const CampaignId& campaign_id, bool execute) {
  if (shutting_down_.load()) {
    return make_error(ErrorCode::cancelled, "the engine has been shut down");
  }
  const Timestamp now = deps_.clock->now();
  const std::lock_guard<std::mutex> lock(mutex_);
  const CampaignRecord* stored = store_.campaign(campaign_id);
  if (stored == nullptr) {
    return make_error(ErrorCode::not_found, "unknown campaign", campaign_id.str());
  }
  CampaignRecord campaign = *stored;
  const std::vector<AttemptRecord> attempts = store_.attempts_for(campaign_id);

  if (campaign.state == UpgradeState::rolled_back) {
    return make_error(ErrorCode::precondition_failed, "the campaign is already rolled back");
  }
  bool irreversible_executed = false;
  std::size_t touched = 0;
  for (const auto& attempt : attempts) {
    // An attempt that changed the target counts even when the change was later
    // abandoned or failed verification: the target is on the new version.
    if (attempt.state == AttemptState::activated || attempt.state == AttemptState::verified ||
        attempt.state == AttemptState::rolling_back ||
        (attempt.state == AttemptState::failed && attempt.activation_observed) ||
        (attempt.state == AttemptState::abandoned && attempt.activation_observed) ||
        (attempt.state == AttemptState::aborted && attempt.activation_observed)) {
      ++touched;
      if (!campaign.artifact.rollback().reversible) {
        irreversible_executed = true;
      }
    }
  }
  if (campaign.strategy == StrategyKind::control_plane_generation_handoff) {
    for (const auto& attempt : attempts) {
      if (attempt.state == AttemptState::verified) {
        irreversible_executed = true;
      }
    }
  }
  if (irreversible_executed) {
    const Decision decision = make_decision_locked(
        DecisionKind::rollback, campaign, DecisionOutcome::deny, "refuse rollback",
        "the campaign crossed an irreversible boundary and cannot be reversed",
        {DecisionInput{"rollback_eligible", "false"},
         DecisionInput{"boundary", campaign.artifact.rollback().boundary}},
        {RejectedAlternative{"roll the targets back",
                             "the declared irreversible boundary forbids returning to the "
                             "previous version"}});
    record_decision_locked(decision);
    return make_error(ErrorCode::irreversible,
                      "rollback is refused: the campaign crossed an irreversible boundary",
                      campaign.artifact.rollback().boundary);
  }
  if (!campaign.artifact.rollback().reversible) {
    return make_error(ErrorCode::irreversible,
                      "rollback is refused: the artifact declares the upgrade irreversible",
                      campaign.artifact.rollback().boundary);
  }
  bool adapters_support_rollback = true;
  for (const auto& stage : campaign.plan.stages) {
    for (const auto& target_id : stage.targets) {
      const TargetDescriptor* descriptor = last_inventory_.find(target_id);
      if (descriptor == nullptr || !descriptor->claims.supports_rollback) {
        adapters_support_rollback = false;
      }
    }
  }
  if (!adapters_support_rollback) {
    return make_error(ErrorCode::unsupported,
                      "rollback is refused: a target adapter does not claim rollback support");
  }
  if (touched == 0) {
    return make_error(ErrorCode::precondition_failed,
                      "rollback is refused: no target has been activated by this campaign");
  }

  TransitionGuards guards = guards_locked(campaign);
  guards.rollback_eligible = true;
  guards.failure_recorded = true;
  if (campaign.state != UpgradeState::failed) {
    if (campaign.state == UpgradeState::proposed) {
      return make_error(ErrorCode::precondition_failed,
                        "a proposed campaign has nothing to roll back");
    }
    FUM_TRYV(transition_locked(campaign, UpgradeState::failed, guards));
  }
  FUM_TRYV(transition_locked(campaign, UpgradeState::rollback_planned, guards));
  if (execute) {
    FUM_TRYV(transition_locked(campaign, UpgradeState::rolling_back, guards));
  }
  const Decision decision = make_decision_locked(
      DecisionKind::rollback, campaign, DecisionOutcome::allow,
      execute ? "execute the recorded rollback plan" : "plan a rollback",
      campaign.rollback_plan.empty() ? "restore the previous version on every activated target"
                                     : campaign.rollback_plan,
      {DecisionInput{"activated_targets", std::to_string(touched)},
       DecisionInput{"rollback_artifact", campaign.rollback_artifact}},
      {RejectedAlternative{"leave the targets on the new version",
                           "the operator requested a rollback and it is eligible"}});
  record_decision_locked(decision);

  PreflightReport report;
  report.campaign = campaign.id;
  report.generation = campaign.generation;
  report.incarnation = incarnation_;
  report.epoch = campaign.epoch;
  report.passed = true;
  report.issued_at = now;
  report.expires_at = now;
  report.policy = policy_.id.str();
  report.policy_revision = policy_.revision;
  report.fingerprint = campaign.rollback_plan;
  return report;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------
Result<CampaignStatus> Engine::status(const CampaignId& campaign_id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const CampaignRecord* stored = store_.campaign(campaign_id);
  if (stored == nullptr) {
    return make_error(ErrorCode::not_found, "unknown campaign", campaign_id.str());
  }
  CampaignStatus status;
  status.campaign = *stored;
  status.attempts = store_.attempts_for(campaign_id);
  for (const auto& attempt : status.attempts) {
    if (attempt_state_is_in_flight(attempt.state)) {
      ++status.in_flight;
    }
  }
  const Timestamp now = deps_.clock->now();
  status.ticket_current = ticket_current_locked(status.campaign, now, status.ticket_detail);
  status.skew = assess_current_skew_locked(status.campaign);
  if (const auto* decisions = store_.decisions(campaign_id); decisions != nullptr) {
    status.decisions = *decisions;
  }
  return status;
}

Result<std::vector<CampaignStatus>> Engine::campaigns() const {
  std::vector<CampaignStatus> out;
  const std::lock_guard<std::mutex> lock(mutex_);
  const Timestamp now = deps_.clock->now();
  for (const auto& record : store_.campaigns()) {
    CampaignStatus status;
    status.campaign = record;
    status.attempts = store_.attempts_for(record.id);
    for (const auto& attempt : status.attempts) {
      if (attempt_state_is_in_flight(attempt.state)) {
        ++status.in_flight;
      }
    }
    status.ticket_current = ticket_current_locked(record, now, status.ticket_detail);
    status.skew = assess_current_skew_locked(record);
    out.push_back(std::move(status));
  }
  return out;
}

Result<std::string> Engine::explain(const CampaignId& campaign_id) const {
  auto status_result = status(campaign_id);
  if (!status_result.has_value()) {
    return status_result.error();
  }
  const CampaignStatus& status = status_result.value();
  const CampaignRecord& campaign = status.campaign;
  std::string out;
  out.append("campaign " + campaign.id.str() + "\n");
  out.append("  component: " + campaign.component.str() + "\n");
  out.append("  state: " + std::string(upgrade_state_name(campaign.state)) + "\n");
  out.append("  strategy: " + std::string(strategy_kind_name(campaign.strategy)) + "\n");
  out.append("  artifact: " + campaign.artifact.id().str() + " version " +
             campaign.artifact.version().text() + " build " + campaign.artifact.build().str() +
             "\n");
  out.append("  digest: " + campaign.artifact.digest().hex() + "\n");
  out.append("  generation: " + std::to_string(campaign.generation.value()) +
             " epoch: " + std::to_string(campaign.epoch.value()) +
             " authority: " + campaign.authority.str() + "\n");
  out.append("  owner incarnation: " + std::to_string(campaign.owner_incarnation.value()) +
             " (current " + std::to_string(incarnation_.value()) + ")\n");
  out.append("  plan fingerprint: " + campaign.plan.fingerprint() + "\n");
  if (!campaign.plan.executable()) {
    out.append("  plan rejections:\n");
    for (const auto& rejection : campaign.plan.rejections) {
      out.append("    " + std::string(plan_rejection_name(rejection.code)) + " [" +
                 rejection.subject + "] " + rejection.detail + "\n");
    }
  }
  out.append("  stages:\n");
  for (std::size_t i = 0; i < campaign.stages.size(); ++i) {
    const auto& stage = campaign.stages[i];
    out.append("    " + std::to_string(i + 1) + ". " + stage.id.str() + " state=" +
               stage_state_name(stage.state));
    if (i == campaign.active_stage) {
      out.append(" (active)");
    }
    out.append("\n");
  }
  out.append("  attempts:\n");
  for (const auto& attempt : status.attempts) {
    out.append("    " + attempt.id.str() + " target=" + attempt.target.str() + " state=" +
               attempt_state_name(attempt.state) + " retries=" + std::to_string(attempt.retries));
    if (!attempt.last_error.empty()) {
      out.append(" error=" + attempt.last_error);
    }
    out.append("\n");
  }
  out.append("  rollback: eligible=" + std::string(campaign.rollback_eligible ? "true" : "false") +
             " plan=" + campaign.rollback_plan + "\n");
  if (!campaign.plan.irreversible_steps.empty()) {
    out.append("  irreversible steps: " + step_list(campaign.plan.irreversible_steps) +
               " acknowledged=" +
               std::string(campaign.irreversible_acknowledged ? "true" : "false") + "\n");
  }
  out.append("  ticket: " + status.ticket_detail + "\n");
  out.append("  skew: " + status.skew.summary + "\n");
  if (!campaign.block_reason.empty()) {
    out.append("  blocked: " + campaign.block_reason + "\n");
  }
  if (!campaign.failure_reason.empty()) {
    out.append("  failure: " + campaign.failure_reason + "\n");
  }
  if (!status.decisions.empty()) {
    out.append("  decisions:\n");
    for (const auto& decision : status.decisions) {
      out.append("    [" + std::string(decision_outcome_name(decision.outcome)) + "] " +
                 decision_kind_name(decision.kind) + " " + decision.selected + " -- " +
                 decision.rationale + " (fingerprint " + decision.fingerprint().substr(0, 16) +
                 ")\n");
    }
  }
  return out;
}

Result<InventorySnapshot> Engine::inventory() {
  const Timestamp now = deps_.clock->now();
  FUM_TRYV(refresh_inventory(now, false));
  const std::lock_guard<std::mutex> lock(mutex_);
  return last_inventory_;
}

Result<std::vector<ProvenanceRecord>> Engine::provenance(const CampaignId& campaign_id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ProvenanceRecord> out;
  for (const auto& record : store_.provenance()) {
    if (campaign_id.empty() || record.campaign == campaign_id) {
      out.push_back(record);
    }
  }
  const LedgerVerification verification = verify_ledger(store_.provenance());
  if (!verification.valid) {
    return make_error(ErrorCode::integrity_failure, "the provenance ledger failed verification",
                      verification.reason);
  }
  return out;
}

Result<std::vector<Decision>> Engine::decisions(const CampaignId& campaign_id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto* decisions = store_.decisions(campaign_id);
  if (decisions == nullptr) {
    return std::vector<Decision>{};
  }
  return *decisions;
}

ReconcileReport Engine::reconciliation() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return reconciliation_;
}

json::Value Engine::stats() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  json::Value value = json::Value::make_object();
  value.set("incarnation", json::Value::make_uint(incarnation_.value()));
  value.set("epoch", json::Value::make_uint(epoch_.value()));
  value.set("executor", json::Value::make_string(executor_kind_name(config_.executor)));
  value.set("store", store_.stats().to_json());
  value.set("recovery", store_.recovery().to_json());
  value.set("reconciliation", reconciliation_.to_json());
  value.set("in_flight", json::Value::make_bool(has_in_flight_locked()));
  json::Value adapters = json::Value::make_array();
  for (const auto& adapter : deps_.adapters.all()) {
    json::Value entry = json::Value::make_object();
    entry.set("adapter_id", json::Value::make_string(adapter->id().str()));
    entry.set("kind", json::Value::make_string(adapter_kind_name(adapter->kind())));
    json::Value strategies = json::Value::make_array();
    for (const auto strategy :
         {StrategyKind::in_place, StrategyKind::restart_based,
          StrategyKind::redundant_pair_rolling,
          StrategyKind::control_plane_generation_handoff}) {
      if (adapter->claims().supports(strategy)) {
        strategies.push(json::Value::make_string(strategy_kind_name(strategy)));
      }
    }
    entry.set("strategies", std::move(strategies));
    adapters.push(std::move(entry));
  }
  value.set("adapters", std::move(adapters));
  if (pool_) {
    const WorkerPool::Stats pool = pool_->stats();
    json::Value pool_json = json::Value::make_object();
    pool_json.set("threads", json::Value::make_uint(pool_->threads()));
    pool_json.set("submitted", json::Value::make_uint(pool.submitted));
    pool_json.set("completed", json::Value::make_uint(pool.completed));
    pool_json.set("rejected", json::Value::make_uint(pool.rejected));
    pool_json.set("discarded", json::Value::make_uint(pool.discarded));
    pool_json.set("pending", json::Value::make_uint(pool_->pending()));
    value.set("worker_pool", std::move(pool_json));
  }
  const LedgerVerification ledger = verify_ledger(store_.provenance());
  json::Value ledger_json = json::Value::make_object();
  ledger_json.set("valid", json::Value::make_bool(ledger.valid));
  ledger_json.set("records", json::Value::make_uint(ledger.records));
  ledger_json.set("reason", json::Value::make_string(ledger.reason));
  value.set("provenance_ledger", std::move(ledger_json));
  return value;
}

}  // namespace fum
