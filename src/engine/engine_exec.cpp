// Engine: work planning, fenced execution and commits.
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <utility>

#include "fum/engine/engine.hpp"
#include "fum/core/hash.hpp"

namespace fum {
namespace {

// Batch completion latch: waits for real work, never on a clock.
class BatchLatch {
 public:
  explicit BatchLatch(std::size_t count) : remaining_(count) {}
  void done() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (remaining_ > 0) {
      --remaining_;
    }
    if (remaining_ == 0) {
      cv_.notify_all();
    }
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return remaining_ == 0; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t remaining_;
};

AttemptState expected_state_for(OperationKind kind) {
  switch (kind) {
    case OperationKind::prepare: return AttemptState::preparing;
    case OperationKind::activate: return AttemptState::activating;
    case OperationKind::verify: return AttemptState::verifying;
    case OperationKind::rollback: return AttemptState::rolling_back;
  }
  return AttemptState::failed;
}

DecisionKind decision_kind_for(UpgradeState state) {
  switch (state) {
    case UpgradeState::validated: return DecisionKind::admit;
    case UpgradeState::prepared: return DecisionKind::stage;
    case UpgradeState::staged: return DecisionKind::stage;
    case UpgradeState::activating: return DecisionKind::activate;
    case UpgradeState::verifying: return DecisionKind::verify;
    case UpgradeState::completed: return DecisionKind::verify;
    case UpgradeState::blocked: return DecisionKind::block;
    case UpgradeState::paused: return DecisionKind::pause;
    case UpgradeState::failed: return DecisionKind::block;
    case UpgradeState::rollback_planned: return DecisionKind::rollback;
    case UpgradeState::rolling_back: return DecisionKind::rollback;
    case UpgradeState::rolled_back: return DecisionKind::rollback;
    case UpgradeState::proposed: return DecisionKind::plan;
  }
  return DecisionKind::plan;
}

bool is_retryable(ErrorCode code) {
  return code == ErrorCode::timeout || code == ErrorCode::io_error ||
         code == ErrorCode::internal || code == ErrorCode::resource_exhausted;
}

std::string join_failures(const std::vector<std::string>& failures) {
  std::string out;
  for (std::size_t i = 0; i < failures.size() && i < 4; ++i) {
    if (i != 0) {
      out.append("; ");
    }
    out.append(failures[i]);
  }
  if (failures.size() > 4) {
    out.append("; ...");
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------
Decision Engine::make_decision_locked(DecisionKind kind, const CampaignRecord& campaign,
                                      DecisionOutcome outcome, std::string selected,
                                      std::string rationale, std::vector<DecisionInput> inputs,
                                      std::vector<RejectedAlternative> rejected,
                                      std::vector<DecisionEvidenceRef> evidence) const {
  Decision decision;
  const Sequence sequence = decision_sequence_;
  decision.id = make_DecisionId("dec-" + std::to_string(incarnation_.value()) + "-" +
                                std::to_string(sequence.value() + 1));
  decision.kind = kind;
  decision.outcome = outcome;
  decision.campaign = campaign.id;
  decision.generation = campaign.generation;
  decision.authority = campaign.authority;
  decision.incarnation = incarnation_;
  decision.epoch = campaign.epoch;
  decision.policy = policy_.id;
  decision.policy_revision = policy_.revision;
  decision.inputs = std::move(inputs);
  decision.evidence = std::move(evidence);
  decision.selected = std::move(selected);
  decision.rejected = std::move(rejected);
  decision.rationale = std::move(rationale);
  decision.at = deps_.clock->now();
  return decision;
}

void Engine::record_decision_locked(Decision decision) {
  const Timestamp now = deps_.clock->now();
  if (decision.at.unix_nanos() == 0) {
    decision.at = now;
  }
  if (auto next = decision_sequence_.try_next(); next.has_value()) {
    decision_sequence_ = next.value();
  }
  static_cast<void>(store_.append_decision(decision, now));
}

// ---------------------------------------------------------------------------
// Guards
// ---------------------------------------------------------------------------
bool Engine::ticket_current_locked(const CampaignRecord& campaign, Timestamp now,
                                   std::string& detail) const {
  const PreflightTicket& ticket = campaign.ticket;
  if (!ticket.passed) {
    detail = "no passing preflight ticket is recorded";
    return false;
  }
  if (ticket.generation != campaign.generation) {
    detail = "the ticket was issued for generation " + std::to_string(ticket.generation.value()) +
             " but the campaign is at generation " + std::to_string(campaign.generation.value());
    return false;
  }
  if (ticket.incarnation != incarnation_) {
    detail = "the ticket was issued in incarnation " +
             std::to_string(ticket.incarnation.value()) + " but this incarnation is " +
             std::to_string(incarnation_.value());
    return false;
  }
  if (ticket.epoch != epoch_) {
    detail = "the ticket was issued under epoch " + std::to_string(ticket.epoch.value()) +
             " but the current epoch is " + std::to_string(epoch_.value());
    return false;
  }
  if (!ticket.expires_at.after(now)) {
    detail = "the ticket expired at " + ticket.expires_at.to_iso8601();
    return false;
  }
  if (!last_integrity_.verified || last_integrity_.declared != campaign.artifact.digest()) {
    detail = "artifact integrity is not verified for this digest in the current incarnation";
    return false;
  }
  detail = "ticket is current";
  return true;
}

SkewAssessment Engine::assess_current_skew_locked(const CampaignRecord& campaign) const {
  const Version newest = campaign.artifact.version();
  const std::vector<AttemptRecord> attempts = store_.attempts_for(campaign.id);
  std::vector<std::pair<std::string, Version>> live;
  std::size_t known = 0;   // targets whose live version is known in this incarnation
  for (const auto& stage : campaign.plan.stages) {
    for (const auto& target_id : stage.targets) {
      bool upgraded = false;
      for (const auto& attempt : attempts) {
        if (attempt.target == target_id &&
            (attempt.state == AttemptState::verified || attempt.state == AttemptState::activated)) {
          upgraded = true;
          break;
        }
      }
      if (upgraded) {
        ++known;
        live.emplace_back(target_id.str(), newest);
        continue;
      }
      const TargetDescriptor* descriptor = last_inventory_.find(target_id);
      if (descriptor != nullptr) {
        ++known;
      }
      live.emplace_back(target_id.str(),
                        descriptor != nullptr ? descriptor->installed_version : Version{});
    }
  }
  SkewAssessment assessment = assess_skew(campaign.plan.skew_budget, live, newest);
  if (known == 0 && !live.empty()) {
    // Without an inventory observation nothing about the live mix can be
    // proven, so the budget is reported as unproven rather than satisfied.
    assessment.within_budget = false;
    assessment.summary =
        "skew could not be assessed: no inventory has been resolved in this incarnation";
  }
  return assessment;
}

TransitionGuards Engine::guards_locked(const CampaignRecord& campaign) {
  TransitionGuards guards;
  const Timestamp now = deps_.clock->now();
  std::string detail;
  guards.preflight_current = ticket_current_locked(campaign, now, detail);
  guards.artifact_integrity_verified =
      last_integrity_.verified && last_integrity_.declared == campaign.artifact.digest();
  guards.rollback_plan_recorded = !campaign.rollback_plan.empty();
  guards.irreversible_acknowledged =
      campaign.plan.irreversible_steps.empty() || campaign.irreversible_acknowledged;
  guards.rollback_eligible = campaign.rollback_eligible;
  guards.failure_recorded = !campaign.failure_reason.empty();

  guards.adapter_supports_strategy = true;
  for (const auto& stage : campaign.plan.stages) {
    for (const auto& target_id : stage.targets) {
      const TargetDescriptor* descriptor = last_inventory_.find(target_id);
      if (descriptor == nullptr || !descriptor->claims.supports(campaign.plan.strategy)) {
        guards.adapter_supports_strategy = false;
      }
    }
  }

  bool all_stages_complete = !campaign.stages.empty();
  for (const auto& stage : campaign.stages) {
    if (stage.state != StageState::completed) {
      all_stages_complete = false;
    }
  }
  guards.all_stages_complete = all_stages_complete;

  bool staging_complete = false;
  if (campaign.active_stage < campaign.stages.size()) {
    staging_complete = campaign.stages[campaign.active_stage].state == StageState::prepared;
  }
  guards.staging_complete = staging_complete;

  const SkewAssessment skew = assess_current_skew_locked(campaign);
  guards.skew_within_budget = skew.within_budget;

  guards.drain_satisfied = true;
  if (campaign.active_stage < campaign.plan.stages.size()) {
    const StagePlan& stage = campaign.plan.stages[campaign.active_stage];
    if (stage.requires_service_removal && deps_.drain) {
      guards.drain_satisfied = drain_tickets_.find(stage.id.str()) != drain_tickets_.end();
    }
  }

  // Rollback completeness is campaign-wide: every attempt that reached an
  // activated state must have been rolled back, and at least one must have been.
  {
    const std::vector<AttemptRecord> all = store_.attempts_for(campaign.id);
    bool any_pending = false;
    bool any_rolled_back = false;
    for (const auto& attempt : all) {
      if (attempt.state == AttemptState::activated || attempt.state == AttemptState::verified ||
          attempt.state == AttemptState::rolling_back ||
          (attempt.state == AttemptState::failed && attempt.activation_observed) ||
          (attempt.state == AttemptState::abandoned && attempt.activation_observed) ||
          (attempt.state == AttemptState::aborted && attempt.activation_observed)) {
        any_pending = true;
      }
      if (attempt.state == AttemptState::rolled_back) {
        any_rolled_back = true;
      }
    }
    guards.rollback_complete = any_rolled_back && !any_pending;
  }

  bool activation_complete = false;
  bool verification_fresh = false;
  if (campaign.active_stage < campaign.plan.stages.size()) {
    const StagePlan& stage = campaign.plan.stages[campaign.active_stage];
    const std::vector<AttemptRecord> attempts = store_.attempts_for(campaign.id);
    activation_complete = !stage.targets.empty();
    verification_fresh = !stage.targets.empty();
    for (const auto& target_id : stage.targets) {
      const AttemptRecord* latest = nullptr;
      for (const auto& attempt : attempts) {
        if (attempt.stage == stage.id && attempt.target == target_id) {
          if (latest == nullptr || latest->id < attempt.id) {
            latest = &attempt;
          }
        }
      }
      if (latest == nullptr) {
        activation_complete = false;
        verification_fresh = false;
        continue;
      }
      if (latest->state != AttemptState::activated && latest->state != AttemptState::verified) {
        activation_complete = false;
      }
      const FreshnessVerdict freshness =
          evaluate_freshness(latest->evidence_at, latest->evidence_validity, latest->owner, now,
                             incarnation_);
      if (latest->state != AttemptState::verified || !freshness.fresh) {
        verification_fresh = false;
      }
    }
  }
  guards.activation_complete = activation_complete;
  guards.verification_fresh_passed = verification_fresh;
  return guards;
}

Status Engine::transition_locked(CampaignRecord& campaign, UpgradeState to,
                                 const TransitionGuards& guards) {
  const TransitionVerdict verdict = evaluate_transition(campaign.state, to, guards);
  if (!verdict.allowed) {
    const Decision decision = make_decision_locked(
        DecisionKind::block, campaign, DecisionOutcome::deny,
        std::string("transition to ") + upgrade_state_name(to), verdict.explain());
    record_decision_locked(decision);
    return make_error(ErrorCode::precondition_failed,
                      std::string("transition refused: ") + upgrade_state_name(campaign.state) +
                          " -> " + upgrade_state_name(to),
                      verdict.explain());
  }
  if (campaign.state == to) {
    return ok_status();
  }
  const Timestamp now = deps_.clock->now();
  campaign.state = to;
  if (auto revision = campaign.revision.try_next(); revision.has_value()) {
    campaign.revision = revision.value();
  }
  campaign.updated_at = now;
  if (to == UpgradeState::completed || to == UpgradeState::rolled_back) {
    campaign.block_reason.clear();
    campaign.failure_reason.clear();
  }
  FUM_TRYV(store_.put_campaign(campaign, now));
  const Decision decision =
      make_decision_locked(decision_kind_for(to), campaign, DecisionOutcome::allow,
                           std::string("enter ") + upgrade_state_name(to), verdict.reason);
  record_decision_locked(decision);
  return ok_status();
}

AttemptId Engine::allocate_attempt_locked(const CampaignRecord& campaign, const StageId& stage,
                                          const TargetId& target) {
  const Sequence sequence = attempt_sequence_;
  if (auto next = attempt_sequence_.try_next(); next.has_value()) {
    attempt_sequence_ = next.value();
  }
  return make_AttemptId(campaign.id.str() + "/" + stage.str() + "/" + target.str() + "/a" +
                        std::to_string(sequence.value() + 1));
}

Status Engine::append_provenance_locked(const WorkItem& item, ProvenanceOutcome outcome,
                                        std::string detail, Timestamp now) {
  auto sequence = store_.provenance_sequence().try_next();
  if (!sequence.has_value()) {
    return sequence.error();
  }
  ProvenanceRecord record;
  record.id = make_RecordId(item.campaign.str() + "/" + item.target.str() + "/" +
                            std::to_string(sequence.value().value()));
  record.campaign = item.campaign;
  record.generation = item.generation;
  record.stage = item.stage;
  record.attempt = item.attempt;
  record.target = item.target;
  record.component = item.artifact.component();
  record.artifact = item.artifact.id();
  record.build = item.artifact.build();
  record.from_version = item.target_descriptor.installed_version;
  record.to_version = item.artifact.version();
  record.digest = item.artifact.digest();
  record.outcome = outcome;
  record.authority = item.target_descriptor.authority;
  record.incarnation = incarnation_;
  record.recorded_at = now;
  record.detail = std::move(detail);
  FUM_TRYV(store_.append_provenance(record, now));
  return ok_status();
}

Status Engine::check_fence_locked(const WorkItem& item, AttemptState expected) const {
  const AttemptRecord* attempt = store_.attempt(item.attempt);
  if (attempt == nullptr) {
    return make_error(ErrorCode::fenced, "the attempt no longer exists", item.attempt.str());
  }
  if (attempt->generation != item.generation) {
    return make_error(ErrorCode::fenced, "the attempt belongs to an older campaign generation",
                      std::to_string(attempt->generation.value()) + " != " +
                          std::to_string(item.generation.value()));
  }
  if (attempt->owner != incarnation_) {
    return make_error(ErrorCode::fenced, "the attempt is owned by another incarnation",
                      std::to_string(attempt->owner.value()) + " != " +
                          std::to_string(incarnation_.value()));
  }
  if (attempt->state != expected) {
    return make_error(ErrorCode::fenced, "the attempt state moved",
                      std::string(attempt_state_name(attempt->state)) + " != " +
                          attempt_state_name(expected));
  }
  const CampaignRecord* campaign = store_.campaign(item.campaign);
  if (campaign == nullptr) {
    return make_error(ErrorCode::fenced, "the campaign no longer exists", item.campaign.str());
  }
  if (campaign->generation != item.generation) {
    return make_error(ErrorCode::fenced, "the campaign generation moved",
                      std::to_string(campaign->generation.value()) + " != " +
                          std::to_string(item.generation.value()));
  }
  if (campaign->epoch != item.epoch) {
    return make_error(ErrorCode::fenced, "the campaign epoch moved",
                      std::to_string(campaign->epoch.value()) + " != " +
                          std::to_string(item.epoch.value()));
  }
  const UpgradeState state = campaign->state;
  bool allowed = false;
  switch (item.kind) {
    case OperationKind::prepare:
      allowed = state == UpgradeState::prepared || state == UpgradeState::staged;
      break;
    case OperationKind::activate:
      allowed = state == UpgradeState::staged || state == UpgradeState::activating;
      break;
    case OperationKind::verify:
      allowed = state == UpgradeState::activating || state == UpgradeState::verifying;
      break;
    case OperationKind::rollback:
      allowed = state == UpgradeState::rolling_back;
      break;
  }
  if (!allowed) {
    return make_error(ErrorCode::fenced, "the campaign state no longer accepts this operation",
                      std::string(operation_kind_name(item.kind)) + " in state " +
                          upgrade_state_name(state));
  }
  return ok_status();
}

Status Engine::fail_locked(CampaignRecord& campaign, const std::string& reason) {
  campaign.failure_reason = reason;
  TransitionGuards guards = guards_locked(campaign);
  guards.failure_recorded = true;
  const UpgradeState target =
      campaign.state == UpgradeState::proposed ? UpgradeState::blocked : UpgradeState::failed;
  const Status status = transition_locked(campaign, target, guards);
  if (!status.has_value() && status.error().code() != ErrorCode::precondition_failed) {
    return status;
  }
  return ok_status();
}

// ---------------------------------------------------------------------------
// Work planning
// ---------------------------------------------------------------------------
void Engine::plan_work_locked(std::vector<WorkItem>& out, std::size_t budget) {
  const Timestamp now = deps_.clock->now();
  std::vector<CampaignRecord> campaign_list = store_.campaigns();
  for (CampaignRecord& campaign : campaign_list) {
    if (out.size() >= budget || shutting_down_.load()) {
      return;
    }
    const bool running = campaign.state == UpgradeState::prepared ||
                         campaign.state == UpgradeState::staged ||
                         campaign.state == UpgradeState::activating ||
                         campaign.state == UpgradeState::verifying ||
                         campaign.state == UpgradeState::rolling_back;
    if (!running || campaign.active_stage >= campaign.plan.stages.size()) {
      continue;
    }
    const StagePlan& stage = campaign.plan.stages[campaign.active_stage];

    // A campaign that was resumed with its current stage already prepared goes
    // straight back to staging; no duplicate preparation work is emitted.
    if (campaign.state == UpgradeState::prepared &&
        campaign.stages[campaign.active_stage].state == StageState::prepared) {
      TransitionGuards guards = guards_locked(campaign);
      guards.staging_complete = true;
      static_cast<void>(transition_locked(campaign, UpgradeState::staged, guards));
      // Fall through: the same pass may emit the activation work for this stage.
    }

    // A campaign that resumes with attempts already activated moves forward
    // instead of waiting for work that will never be emitted.
    if (campaign.state == UpgradeState::staged || campaign.state == UpgradeState::activating) {
      const TransitionGuards guards = guards_locked(campaign);
      if (guards.activation_complete) {
        if (campaign.state == UpgradeState::staged) {
          static_cast<void>(transition_locked(campaign, UpgradeState::activating, guards));
        }
        static_cast<void>(transition_locked(campaign, UpgradeState::verifying, guards));
      }
    }

    // Rollback spans every stage that was touched, not only the active one.
    if (campaign.state == UpgradeState::rolling_back) {
      for (const auto& stage_plan : campaign.plan.stages) {
        for (const auto& target_id : stage_plan.targets) {
          if (out.size() >= budget) {
            return;
          }
          const TargetDescriptor* descriptor = last_inventory_.find(target_id);
          if (descriptor == nullptr) {
            continue;
          }
          std::vector<AttemptRecord> rollback_attempts = store_.attempts_for(campaign.id);
          const AttemptRecord* latest = nullptr;
          for (const auto& attempt : rollback_attempts) {
            if (attempt.stage == stage_plan.id && attempt.target == target_id) {
              if (latest == nullptr || latest->id < attempt.id) {
                latest = &attempt;
              }
            }
          }
          const bool needs_rollback =
              latest != nullptr &&
              (latest->state == AttemptState::verified ||
               latest->state == AttemptState::activated ||
               (latest->state == AttemptState::failed && latest->activation_observed));
          if (!needs_rollback) {
            continue;
          }
          AttemptRecord updated = *latest;
          updated.state = AttemptState::rolling_back;
          updated.updated_at = now;
          updated.owner = incarnation_;
          if (auto revision = updated.revision.try_next(); revision.has_value()) {
            updated.revision = revision.value();
          }
          static_cast<void>(store_.put_attempt(updated, now));
          WorkItem item;
          item.campaign = campaign.id;
          item.generation = campaign.generation;
          item.epoch = campaign.epoch;
          item.stage = stage_plan.id;
          item.target = target_id;
          item.artifact = campaign.artifact;
          item.target_descriptor = *descriptor;
          item.strategy = campaign.strategy;
          item.payload_path = artifact_payload_path_;
          item.kind = OperationKind::rollback;
          item.attempt = updated.id;
          out.push_back(item);
        }
      }
      continue;
    }

    if (campaign.state == UpgradeState::staged) {
      const SkewAssessment skew = assess_current_skew_locked(campaign);
      campaign.last_skew = skew;
      if (!skew.within_budget) {
        campaign.block_reason = "version skew budget exceeded before activation: " + skew.summary;
        TransitionGuards guards = guards_locked(campaign);
        guards.failure_recorded = true;
        if (transition_locked(campaign, UpgradeState::blocked, guards).has_value()) {
          const Decision decision = make_decision_locked(
              DecisionKind::block, campaign, DecisionOutcome::deny, "hold the stage",
              campaign.block_reason,
              {DecisionInput{"worst_distance", std::to_string(skew.worst_distance)},
               DecisionInput{"budget",
                             std::to_string(campaign.plan.skew_budget.max_major_skew) + " major / " +
                                 std::to_string(campaign.plan.skew_budget.max_minor_skew) +
                                 " minor / " +
                                 std::to_string(campaign.plan.skew_budget.max_patch_skew) +
                                 " patch"}},
              {RejectedAlternative{"activate the stage",
                                   "the live version mix would exceed the declared skew budget"}});
          record_decision_locked(decision);
        }
        continue;
      }
    }

    std::vector<AttemptRecord> attempts = store_.attempts_for(campaign.id);
    const auto latest_for = [&attempts, &stage](const TargetId& target) -> const AttemptRecord* {
      const AttemptRecord* latest = nullptr;
      for (const auto& attempt : attempts) {
        if (attempt.stage == stage.id && attempt.target == target) {
          if (latest == nullptr || latest->id < attempt.id) {
            latest = &attempt;
          }
        }
      }
      return latest;
    };
    const auto attempts_for_target = [&attempts, &stage](const TargetId& target) {
      std::size_t count = 0;
      for (const auto& attempt : attempts) {
        if (attempt.stage == stage.id && attempt.target == target) {
          ++count;
        }
      }
      return count;
    };
    const auto count_in_flight = [&attempts, &stage](AttemptState state) {
      std::size_t count = 0;
      for (const auto& attempt : attempts) {
        if (attempt.stage == stage.id && attempt.state == state) {
          ++count;
        }
      }
      return count;
    };

    for (const auto& target_id : stage.targets) {
      if (out.size() >= budget) {
        return;
      }
      const TargetDescriptor* descriptor = last_inventory_.find(target_id);
      if (descriptor == nullptr) {
        continue;
      }
      const AttemptRecord* latest = latest_for(target_id);
      WorkItem item;
      item.campaign = campaign.id;
      item.generation = campaign.generation;
      item.epoch = campaign.epoch;
      item.stage = stage.id;
      item.target = target_id;
      item.artifact = campaign.artifact;
      item.target_descriptor = *descriptor;
      item.strategy = campaign.strategy;
      item.payload_path = artifact_payload_path_;
      item.requires_service_removal = stage.requires_service_removal;
      const auto change = rollout_changes_.find(campaign.id.str());
      if (change != rollout_changes_.end()) {
        item.rollout_change = change->second;
      }

      const bool staging_phase =
          campaign.state == UpgradeState::prepared ||
          (campaign.state == UpgradeState::staged &&
           campaign.stages[campaign.active_stage].state == StageState::pending);
      if (staging_phase) {
        if (latest == nullptr) {
          if (attempts_for_target(target_id) >= policy_.max_attempts_per_target) {
            continue;
          }
          AttemptRecord record;
          record.id = allocate_attempt_locked(campaign, stage.id, target_id);
          record.campaign = campaign.id;
          record.generation = campaign.generation;
          record.stage = stage.id;
          record.target = target_id;
          record.artifact = campaign.artifact.id();
          record.build = campaign.artifact.build();
          record.from_version = descriptor->installed_version;
          record.to_version = campaign.artifact.version();
          record.state = AttemptState::preparing;
          record.owner = incarnation_;
          record.authority = descriptor->authority;
          record.revision = Revision(1);
          record.operation_index = Sequence(1);
          record.started_at = now;
          record.updated_at = now;
          static_cast<void>(store_.put_attempt(record, now));
          attempts.push_back(record);
          item.kind = OperationKind::prepare;
          item.attempt = record.id;
          out.push_back(item);
          continue;
        }
        if (latest->state == AttemptState::preparing && latest->owner == incarnation_) {
          item.kind = OperationKind::prepare;
          item.attempt = latest->id;
          out.push_back(item);
        }
        continue;
      }

      if (latest == nullptr) {
        continue;
      }
      if ((campaign.state == UpgradeState::staged ||
           campaign.state == UpgradeState::activating) &&
          latest->state == AttemptState::prepared) {
        std::size_t limit = std::min<std::size_t>(stage.max_parallel,
                                                  policy_.max_concurrent_activations);
        const std::uint32_t adapter_limit = descriptor->claims.max_parallel_activations == 0
                                                ? 1
                                                : descriptor->claims.max_parallel_activations;
        limit = std::min<std::size_t>(limit, adapter_limit);
        if (count_in_flight(AttemptState::activating) >= limit) {
          continue;
        }
        AttemptRecord updated = *latest;
        updated.state = AttemptState::activating;
        updated.updated_at = now;
        // The claiming incarnation owns the attempt from here on: evidence is
        // re-established under this incarnation before anything is published.
        updated.owner = incarnation_;
        if (auto revision = updated.revision.try_next(); revision.has_value()) {
          updated.revision = revision.value();
        }
        static_cast<void>(store_.put_attempt(updated, now));
        for (auto& attempt : attempts) {
          if (attempt.id == updated.id) {
            attempt.state = AttemptState::activating;
          }
        }
        item.kind = OperationKind::activate;
        item.attempt = updated.id;
        out.push_back(item);
        continue;
      }
      if ((campaign.state == UpgradeState::activating ||
           campaign.state == UpgradeState::verifying) &&
          latest->state == AttemptState::activated) {
        AttemptRecord updated = *latest;
        updated.state = AttemptState::verifying;
        updated.updated_at = now;
        // The claiming incarnation owns the attempt from here on: evidence is
        // re-established under this incarnation before anything is published.
        updated.owner = incarnation_;
        if (auto revision = updated.revision.try_next(); revision.has_value()) {
          updated.revision = revision.value();
        }
        static_cast<void>(store_.put_attempt(updated, now));
        for (auto& attempt : attempts) {
          if (attempt.id == updated.id) {
            attempt.state = AttemptState::verifying;
          }
        }
        item.kind = OperationKind::verify;
        item.attempt = updated.id;
        out.push_back(item);
        continue;
      }
    }
  }
}

}  // namespace fum