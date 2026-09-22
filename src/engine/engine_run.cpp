// Engine: adapter invocation, fenced commits and execution drivers.
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <utility>

#include "fum/engine/engine.hpp"

namespace fum {
namespace {

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

bool is_retryable_error(ErrorCode code) {
  return code == ErrorCode::timeout || code == ErrorCode::io_error ||
         code == ErrorCode::internal || code == ErrorCode::resource_exhausted;
}

StageProgress* find_progress(CampaignRecord& campaign, const StageId& id) {
  for (auto& stage : campaign.stages) {
    if (stage.id == id) {
      return &stage;
    }
  }
  return nullptr;
}

const AttemptRecord* latest_attempt_for(const std::vector<AttemptRecord>& attempts,
                                        const StageId& stage, const TargetId& target) {
  const AttemptRecord* latest = nullptr;
  for (const auto& attempt : attempts) {
    if (attempt.stage == stage && attempt.target == target) {
      if (latest == nullptr || latest->id < attempt.id) {
        latest = &attempt;
      }
    }
  }
  return latest;
}

}  // namespace

// ---------------------------------------------------------------------------
// Adapter invocation
// ---------------------------------------------------------------------------
Status Engine::execute_item(const WorkItem& item) {
  const Timestamp now = deps_.clock->now();
  OperationOutcome outcome;
  outcome.kind = item.kind;
  const auto adapter = deps_.adapters.find(item.target_descriptor.adapter);
  if (!adapter) {
    switch (item.kind) {
      case OperationKind::prepare:
        outcome.prepare.detail = "no adapter is registered for " + item.target_descriptor.adapter.str();
        break;
      case OperationKind::activate:
        outcome.activate.detail = "no adapter is registered for " + item.target_descriptor.adapter.str();
        break;
      case OperationKind::verify:
        outcome.verify.detail = "no adapter is registered for " + item.target_descriptor.adapter.str();
        break;
      case OperationKind::rollback:
        outcome.rollback.detail = "no adapter is registered for " + item.target_descriptor.adapter.str();
        break;
    }
    return commit_item(item, outcome);
  }

  TargetOperation operation;
  operation.fence.campaign = item.campaign;
  operation.fence.generation = item.generation;
  operation.fence.incarnation = incarnation_;
  operation.fence.attempt = item.attempt;
  operation.fence.authority = item.target_descriptor.authority;
  operation.key.campaign = item.campaign;
  operation.key.generation = item.generation;
  operation.key.stage = item.stage;
  operation.key.target = item.target;
  operation.key.operation = operation_kind_name(item.kind);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const AttemptRecord* attempt = store_.attempt(item.attempt);
    operation.key.index = attempt != nullptr ? attempt->operation_index : Sequence(1);
  }
  operation.target = item.target_descriptor;
  operation.artifact = item.artifact;
  operation.payload_path = item.payload_path;
  operation.strategy = item.strategy;
  operation.now = now;
  operation.epoch = item.epoch;

  // Service removal is delegated to the Drain/Maintenance Fabric when the
  // adapter declares that it is required.
  if (item.kind == OperationKind::activate && item.requires_service_removal && deps_.drain) {
    bool have_ticket = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      have_ticket = drain_tickets_.find(item.stage.str()) != drain_tickets_.end();
    }
    if (!have_ticket) {
      DrainRequest request;
      request.target = item.target;
      request.component = item.artifact.component();
      request.reason = "upgrade campaign " + item.campaign.str();
      request.correlation = make_CorrelationId(item.attempt.str());
      request.at = now;
      auto ticket = deps_.drain->acquire(request);
      if (!ticket.has_value()) {
        outcome.gate_refused = true;
        outcome.gate_reason =
            "the Drain/Maintenance Fabric refused a maintenance ticket: " +
            ticket.error().to_string();
        return commit_item(item, outcome);
      }
      const std::lock_guard<std::mutex> lock(mutex_);
      drain_tickets_[item.stage.str()] = ticket.value();
    }
  }

  if (item.kind == OperationKind::activate && policy_.require_health_gate) {
    HealthReport report;
    bool have_report = false;
    if (deps_.health) {
      auto probed = deps_.health->probe(item.target, now, incarnation_);
      if (probed.has_value()) {
        report = std::move(probed).value();
        have_report = true;
      } else {
        outcome.gate_refused = true;
        outcome.gate_reason = "the health probe failed: " + probed.error().to_string();
      }
    } else {
      auto observed = adapter->observe(item.target, now, incarnation_);
      if (observed.has_value()) {
        report.healthy = observed.value().healthy;
        report.detail = observed.value().health_detail;
        report.observed_at = observed.value().observed_at;
        report.incarnation = observed.value().incarnation;
        report.validity = policy_.health_validity;
        report.checks.push_back("adapter observation");
        if (!observed.value().health_known) {
          // The target has no observable health (it is not running yet): the
          // gate is not applicable, and that is recorded rather than assumed.
          report.checks.push_back("health-not-applicable");
          have_report = false;
          outcome.gate_refused = false;
          log(LogLevel::debug, "pre-activation health gate is not applicable",
              {field("target", item.target.str()), field("detail", observed.value().health_detail)});
        } else {
          have_report = true;
        }
      } else {
        outcome.gate_refused = true;
        outcome.gate_reason = "the target could not be observed for a health gate: " +
                              observed.error().to_string();
      }
    }
    if (have_report && !outcome.gate_refused) {
      outcome.health = report;
      outcome.has_health = true;
      const FreshnessVerdict freshness = evaluate_freshness(
          report.observed_at, report.validity, report.incarnation, now, incarnation_);
      if (!freshness.fresh) {
        outcome.gate_refused = true;
        outcome.gate_reason = "health evidence is not fresh: " + freshness.reason;
      } else if (!report.healthy) {
        outcome.gate_refused = true;
        outcome.gate_reason = "the health gate reports the target unhealthy: " + report.detail;
      }
    }
    if (outcome.gate_refused) {
      return commit_item(item, outcome);
    }
  }

  const auto max_retries = policy_.max_retries_per_operation;
  switch (item.kind) {
    case OperationKind::prepare: {
      std::uint32_t retries = 0;
      for (;;) {
        auto result = adapter->prepare(operation);
        if (result.has_value()) {
          outcome.prepare = std::move(result).value();
          break;
        }
        if (!is_retryable_error(result.error().code()) || retries >= max_retries) {
          outcome.prepare.prepared = false;
          outcome.prepare.detail = result.error().to_string();
          break;
        }
        ++retries;
        operation.retry = true;
        log(LogLevel::debug, "retrying prepare",
            {field("target", item.target.str()), field("attempt", std::to_string(retries))});
      }
      break;
    }
    case OperationKind::activate: {
      std::uint32_t retries = 0;
      for (;;) {
        auto result = adapter->activate(operation);
        if (result.has_value()) {
          outcome.activate = std::move(result).value();
          break;
        }
        const ErrorCode code = result.error().code();
        if (!is_retryable_error(code)) {
          outcome.activate.activated = false;
          outcome.activate.detail = result.error().to_string();
          break;
        }
        if (retries >= max_retries) {
          outcome.activate.activated = false;
          outcome.activate.detail = result.error().to_string();
          break;
        }
        ++retries;
        // A lost acknowledgement is not a failure: observe the target before
        // repeating anything. If the desired version is already live, the
        // operation took effect and is reported as such exactly once.
        auto observed = adapter->observe(item.target, now, incarnation_);
        if (observed.has_value() && observed.value().version == item.artifact.version()) {
          outcome.activate.activated = true;
          outcome.activate.already_active = true;
          outcome.activate.observed_version = observed.value().version;
          outcome.activate.observed_build = observed.value().build;
          outcome.skipped_because_applied = true;
          outcome.activate.detail =
              "the activation had already taken effect; the acknowledgement was lost";
          log(LogLevel::warn, "lost acknowledgement resolved by observation",
              {field("target", item.target.str()), field("attempt", std::to_string(retries))});
          break;
        }
        operation.retry = true;
        log(LogLevel::warn, "retrying activate",
            {field("target", item.target.str()), field("attempt", std::to_string(retries))});
      }
      break;
    }
    case OperationKind::verify: {
      std::uint32_t retries = 0;
      for (;;) {
        auto result = adapter->verify(operation);
        if (result.has_value()) {
          outcome.verify = std::move(result).value();
          break;
        }
        if (!is_retryable_error(result.error().code()) || retries >= max_retries) {
          outcome.verify.verified = false;
          outcome.verify.detail = result.error().to_string();
          break;
        }
        ++retries;
        operation.retry = true;
        log(LogLevel::debug, "retrying verify",
            {field("target", item.target.str()), field("attempt", std::to_string(retries))});
      }
      if (outcome.verify.verified && deps_.configuration) {
        ConfigurationDelivery delivery;
        delivery.target = item.target;
        delivery.component = item.artifact.component();
        delivery.version = item.artifact.version();
        delivery.build = item.artifact.build();
        delivery.revision = item.artifact.build().str();
        delivery.at = deps_.clock->now();
        const Status delivered = deps_.configuration->deliver(delivery);
        if (!delivered.has_value()) {
          outcome.verify.verified = false;
          outcome.verify.detail.append("; configuration delivery failed: ");
          outcome.verify.detail.append(delivered.error().to_string());
        }
      }
      break;
    }
    case OperationKind::rollback: {
      std::uint32_t retries = 0;
      for (;;) {
        auto result = adapter->rollback(operation);
        if (result.has_value()) {
          outcome.rollback = std::move(result).value();
          break;
        }
        if (!is_retryable_error(result.error().code()) || retries >= max_retries) {
          outcome.rollback.rolled_back = false;
          outcome.rollback.detail = result.error().to_string();
          break;
        }
        ++retries;
        operation.retry = true;
        log(LogLevel::debug, "retrying rollback",
            {field("target", item.target.str()), field("attempt", std::to_string(retries))});
      }
      break;
    }
  }
  return commit_item(item, outcome);
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------
Status Engine::commit_item(const WorkItem& item, const OperationOutcome& outcome) {
  std::vector<std::pair<LogLevel, std::string>> messages;
  Status result = ok_status();
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const AttemptRecord* attempt = store_.attempt(item.attempt);
    const CampaignRecord* campaign = store_.campaign(item.campaign);
    if (attempt == nullptr || campaign == nullptr) {
      result = make_error(ErrorCode::fenced, "the attempt or campaign disappeared before commit",
                          item.attempt.str());
    } else {
      const Status fence = check_fence_locked(item, expected_state_for(item.kind));
      if (!fence.has_value()) {
        const Decision decision = make_decision_locked(
            DecisionKind::fence, *campaign, DecisionOutcome::deny,
            std::string("reject the stale ") + operation_kind_name(item.kind) + " result",
            fence.error().to_string(),
            {DecisionInput{"attempt", item.attempt.str()},
             DecisionInput{"operation", operation_kind_name(item.kind)}});
        record_decision_locked(decision);
        messages.emplace_back(LogLevel::warn,
                              std::string("fenced stale ") + operation_kind_name(item.kind) +
                                  " commit for " + item.target.str());
        result = fence.error();
      } else {
        result = commit_locked(item, outcome, messages);
      }
    }
  }
  for (const auto& message : messages) {
    log(message.first, message.second, {field("campaign", item.campaign.str()),
                                        field("target", item.target.str())});
  }
  return result;
}

Status Engine::commit_locked(const WorkItem& item, const OperationOutcome& outcome,
                             std::vector<std::pair<LogLevel, std::string>>& messages) {
  const Timestamp now = deps_.clock->now();
  AttemptRecord attempt = *store_.attempt(item.attempt);
  CampaignRecord campaign = *store_.campaign(item.campaign);
  StageProgress* progress = find_progress(campaign, item.stage);

  // Reads attempts from durable state at the moment of evaluation: a snapshot
  // taken before this commit would still show the pre-commit attempt state.
  const auto stage_all = [this, &campaign, &item, progress](auto predicate) {
    if (progress == nullptr) {
      return false;
    }
    const StagePlan* stage_plan = nullptr;
    for (const auto& candidate : campaign.plan.stages) {
      if (candidate.id == item.stage) {
        stage_plan = &candidate;
      }
    }
    if (stage_plan == nullptr || stage_plan->targets.empty()) {
      return false;
    }
    const std::vector<AttemptRecord> current = store_.attempts_for(campaign.id);
    for (const auto& target_id : stage_plan->targets) {
      const AttemptRecord* latest = latest_attempt_for(current, item.stage, target_id);
      if (latest == nullptr || !predicate(latest->state)) {
        return false;
      }
    }
    return true;
  };

  switch (item.kind) {
    case OperationKind::prepare: {
      if (outcome.prepare.prepared || outcome.prepare.already_prepared) {
        attempt.state = AttemptState::prepared;
        attempt.last_error.clear();
        attempt.updated_at = now;
        if (auto revision = attempt.revision.try_next(); revision.has_value()) {
          attempt.revision = revision.value();
        }
        FUM_TRYV(store_.put_attempt(attempt, now));
        if (stage_all([](AttemptState state) {
              return state == AttemptState::prepared || state == AttemptState::activated ||
                     state == AttemptState::verified || state == AttemptState::rolled_back;
            }) &&
            progress != nullptr) {
          if (auto revision = campaign.revision.try_next(); revision.has_value()) {
            campaign.revision = revision.value();
          }
          campaign.updated_at = now;
          for (auto& stage_progress : campaign.stages) {
            if (stage_progress.id == item.stage) {
              stage_progress.state = StageState::prepared;
            }
          }
          FUM_TRYV(store_.put_campaign(campaign, now));
          TransitionGuards guards = guards_locked(campaign);
          if (campaign.state == UpgradeState::prepared) {
            guards.staging_complete = true;
            static_cast<void>(transition_locked(campaign, UpgradeState::staged, guards));
          }
        }
        messages.emplace_back(LogLevel::debug,
                              "prepared " + item.target.str() + " (" + outcome.prepare.detail + ")");
      } else {
        attempt.state = AttemptState::failed;
        attempt.last_error = outcome.prepare.detail;
        attempt.updated_at = now;
        FUM_TRYV(store_.put_attempt(attempt, now));
        messages.emplace_back(LogLevel::error,
                              "prepare failed on " + item.target.str() + ": " +
                                  outcome.prepare.detail);
        FUM_TRYV(fail_locked(campaign, "prepare failed on " + item.target.str() + ": " +
                                           outcome.prepare.detail));
      }
      break;
    }
    case OperationKind::activate: {
      if (outcome.gate_refused) {
        attempt.state = AttemptState::prepared;
        attempt.last_error = outcome.gate_reason;
        attempt.updated_at = now;
        FUM_TRYV(store_.put_attempt(attempt, now));
        campaign.block_reason = outcome.gate_reason;
        TransitionGuards guards = guards_locked(campaign);
        guards.failure_recorded = true;
        static_cast<void>(transition_locked(campaign, UpgradeState::blocked, guards));
        const Decision decision = make_decision_locked(
            DecisionKind::block, campaign, DecisionOutcome::deny, "hold activation",
            outcome.gate_reason,
            {DecisionInput{"target", item.target.str()},
             DecisionInput{"gate", "pre-activation health"}},
            {RejectedAlternative{"activate anyway",
                                 "the health gate is a hard precondition for activation"}});
        record_decision_locked(decision);
        messages.emplace_back(LogLevel::warn,
                              "activation held on " + item.target.str() + ": " + outcome.gate_reason);
        break;
      }
      if (outcome.activate.activated || outcome.activate.already_active) {
        attempt.state = AttemptState::activated;
        attempt.activation_observed = true;
        attempt.last_error.clear();
        attempt.updated_at = now;
        if (auto revision = attempt.revision.try_next(); revision.has_value()) {
          attempt.revision = revision.value();
        }
        FUM_TRYV(store_.put_attempt(attempt, now));
        FUM_TRYV(append_provenance_locked(item, ProvenanceOutcome::applied,
                                          outcome.activate.detail, now));
        if (campaign.state == UpgradeState::staged) {
          TransitionGuards guards = guards_locked(campaign);
          static_cast<void>(transition_locked(campaign, UpgradeState::activating, guards));
          campaign = *store_.campaign(item.campaign);
        }
        const SkewAssessment skew = assess_current_skew_locked(campaign);
        campaign.last_skew = skew;
        if (!skew.within_budget) {
          campaign.block_reason =
              "version skew budget exceeded after activating " + item.target.str() + ": " +
              skew.summary;
          TransitionGuards guards = guards_locked(campaign);
          guards.failure_recorded = true;
          static_cast<void>(transition_locked(campaign, UpgradeState::blocked, guards));
          messages.emplace_back(LogLevel::warn, campaign.block_reason);
          break;
        }
        TransitionGuards guards = guards_locked(campaign);
        if (guards.activation_complete) {
          static_cast<void>(transition_locked(campaign, UpgradeState::verifying, guards));
        }
        messages.emplace_back(LogLevel::info, "activated " + item.target.str() + " with " +
                                                  item.artifact.version().text());
      } else {
        attempt.state = AttemptState::failed;
        attempt.last_error = outcome.activate.detail;
        attempt.updated_at = now;
        FUM_TRYV(store_.put_attempt(attempt, now));
        messages.emplace_back(LogLevel::error,
                              "activation failed on " + item.target.str() + ": " +
                                  outcome.activate.detail);
        FUM_TRYV(fail_locked(campaign, "activation failed on " + item.target.str() + ": " +
                                           outcome.activate.detail));
      }
      break;
    }
    case OperationKind::verify: {
      VerificationInput input;
      input.attempt = &attempt;
      input.artifact = &item.artifact;
      input.outcome = outcome.verify;
      input.policy = &policy_;
      input.now = now;
      input.current_incarnation = incarnation_;
      input.require_health = policy_.require_health_gate;
      input.health = outcome.health;
      const VerificationResult verification = assess_verification(input);
      if (verification.verified) {
        attempt.state = AttemptState::verified;
        attempt.evidence_at = outcome.verify.observed_at;
        attempt.evidence_validity = outcome.verify.validity;
        attempt.observed_version = outcome.verify.observed_version;
        attempt.observed_build = outcome.verify.observed_build;
        attempt.last_error.clear();
        attempt.updated_at = now;
        if (auto revision = attempt.revision.try_next(); revision.has_value()) {
          attempt.revision = revision.value();
        }
        FUM_TRYV(store_.put_attempt(attempt, now));
        FUM_TRYV(append_provenance_locked(item, ProvenanceOutcome::verified, verification.summary,
                                          now));
        messages.emplace_back(LogLevel::info, "verified " + item.target.str() + ": " +
                                                  verification.summary);
        if (stage_all([](AttemptState state) { return state == AttemptState::verified; }) &&
            progress != nullptr) {
          campaign.stages[campaign.active_stage].state = StageState::completed;
          campaign.stages[campaign.active_stage].completed_at = now;
          auto revision = campaign.revision.try_next();
          campaign.revision = revision.has_value() ? revision.value() : campaign.revision;
          campaign.updated_at = now;
          if (campaign.active_stage + 1 < campaign.stages.size()) {
            campaign.active_stage += 1;
            campaign.stages[campaign.active_stage].state = StageState::pending;
            FUM_TRYV(store_.put_campaign(campaign, now));
            TransitionGuards guards = guards_locked(campaign);
            static_cast<void>(transition_locked(campaign, UpgradeState::staged, guards));
            messages.emplace_back(LogLevel::info, "stage completed; advancing to stage " +
                                                      std::to_string(campaign.active_stage + 1));
          } else {
            FUM_TRYV(store_.put_campaign(campaign, now));
            TransitionGuards guards = guards_locked(campaign);
            const Status completed =
                transition_locked(campaign, UpgradeState::completed, guards);
            if (!completed.has_value()) {
              messages.emplace_back(LogLevel::warn,
                                    "completion held: " + completed.error().to_string());
              const Decision decision = make_decision_locked(
                  DecisionKind::verify, campaign, DecisionOutcome::defer,
                  "hold campaign completion", completed.error().to_string(),
                  {DecisionInput{"verification", "fresh and passed"},
                   DecisionInput{"stages", std::to_string(campaign.stages.size())}});
              record_decision_locked(decision);
            } else {
              messages.emplace_back(LogLevel::info, "campaign completed");
            }
          }
        }
      } else if (attempt.retries < policy_.max_retries_per_operation) {
        attempt.retries += 1;
        attempt.state = AttemptState::activated;
        attempt.last_error = join_failures(verification.failures);
        attempt.updated_at = now;
        if (auto index = attempt.operation_index.try_next(); index.has_value()) {
          attempt.operation_index = index.value();
        }
        FUM_TRYV(store_.put_attempt(attempt, now));
        messages.emplace_back(LogLevel::warn, "verification of " + item.target.str() +
                                                  " is being re-attempted: " +
                                                  verification.summary);
      } else {
        attempt.state = AttemptState::failed;
        attempt.last_error = join_failures(verification.failures);
        attempt.updated_at = now;
        FUM_TRYV(store_.put_attempt(attempt, now));
        messages.emplace_back(LogLevel::error, "verification failed on " + item.target.str() +
                                                   ": " + attempt.last_error);
        FUM_TRYV(fail_locked(campaign, "verification failed on " + item.target.str() + ": " +
                                           attempt.last_error));
      }
      break;
    }
    case OperationKind::rollback: {
      if (outcome.rollback.rolled_back || outcome.rollback.already_rolled_back) {
        attempt.state = AttemptState::rolled_back;
        attempt.last_error.clear();
        attempt.updated_at = now;
        if (auto revision = attempt.revision.try_next(); revision.has_value()) {
          attempt.revision = revision.value();
        }
        FUM_TRYV(store_.put_attempt(attempt, now));
        FUM_TRYV(append_provenance_locked(item, ProvenanceOutcome::rolled_back,
                                          outcome.rollback.detail, now));
        messages.emplace_back(LogLevel::info,
                              "rolled back " + item.target.str() + ": " + outcome.rollback.detail);
        TransitionGuards guards = guards_locked(campaign);
        if (guards.rollback_complete) {
          campaign.rollback_performed = true;
          FUM_TRYV(store_.put_campaign(campaign, now));
          static_cast<void>(transition_locked(campaign, UpgradeState::rolled_back, guards));
          messages.emplace_back(LogLevel::info, "campaign rolled back");
        }
      } else {
        attempt.state = AttemptState::failed;
        attempt.last_error = outcome.rollback.detail;
        attempt.updated_at = now;
        FUM_TRYV(store_.put_attempt(attempt, now));
        messages.emplace_back(LogLevel::error, "rollback failed on " + item.target.str() + ": " +
                                                   outcome.rollback.detail);
        campaign.failure_reason = "rollback failed on " + item.target.str() + ": " +
                                  outcome.rollback.detail;
        TransitionGuards guards = guards_locked(campaign);
        guards.failure_recorded = true;
        static_cast<void>(transition_locked(campaign, UpgradeState::failed, guards));
      }
      break;
    }
  }
  return ok_status();
}

// ---------------------------------------------------------------------------
// Drivers
// ---------------------------------------------------------------------------
void Engine::release_finished_drains() {
  std::vector<DrainTicket> released;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (drain_tickets_.empty()) {
      return;
    }
    for (auto it = drain_tickets_.begin(); it != drain_tickets_.end();) {
      bool still_running = false;
      for (const auto& campaign : store_.campaigns()) {
        for (const auto& stage : campaign.stages) {
          if (stage.id.str() == it->first && !stage_state_is_terminal(stage.state)) {
            still_running = true;
          }
        }
      }
      if (still_running) {
        ++it;
        continue;
      }
      released.push_back(it->second);
      it = drain_tickets_.erase(it);
    }
  }
  if (!deps_.drain) {
    return;
  }
  for (const auto& ticket : released) {
    const Status status = deps_.drain->release(ticket);
    if (!status.has_value()) {
      log(LogLevel::warn, "could not release a maintenance ticket", {field("ticket", ticket.ticket)});
    }
  }
}

Status Engine::run_steps(std::size_t max_steps) {
  return run_steps_bounded(max_steps, false);
}

Status Engine::run_steps_bounded(std::size_t max_steps, bool report_exhaustion) {
  if (shutting_down_.load()) {
    return make_error(ErrorCode::cancelled, "the engine is shutting down");
  }
  std::uint64_t admitted_at_start = admissions_.load();
  for (std::size_t step = 0; step < max_steps; ++step) {
    if (shutting_down_.load()) {
      return make_error(ErrorCode::cancelled, "the engine is shutting down");
    }
    std::vector<WorkItem> items;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      plan_work_locked(items, config_.max_work_items_per_pump);
    }
    if (items.empty()) {
      release_finished_drains();
      if (report_exhaustion && admissions_.load() != admitted_at_start) {
        // A campaign was admitted while this pass was draining: keep going.
        admitted_at_start = admissions_.load();
        continue;
      }
      return ok_status();
    }
    if (pool_mode_ && pool_ && pool_->threads() > 1 && items.size() > 1) {
      // Execute the batch concurrently; the calling thread also performs work,
      // so a single-worker pool cannot deadlock here.
      BatchLatch latch(items.size());
      for (std::size_t i = 0; i + 1 < items.size(); ++i) {
        const WorkItem item = items[i];
        const Status submitted = pool_->submit([this, item, &latch] {
          static_cast<void>(execute_item(item));
          latch.done();
        });
        if (!submitted.has_value()) {
          static_cast<void>(execute_item(item));
          latch.done();
        }
      }
      static_cast<void>(execute_item(items.back()));
      latch.done();
      latch.wait();
    } else {
      for (const auto& item : items) {
        static_cast<void>(execute_item(item));
      }
    }
  }
  if (report_exhaustion) {
    return make_error(ErrorCode::resource_exhausted,
                      "the execution driver reached its iteration bound without going idle",
                      std::to_string(max_steps));
  }
  return ok_status();
}

void Engine::driver_loop() {
  Status status = ok_status();
  for (;;) {
    const std::uint64_t admissions = admissions_.load();
    status = run_steps_bounded(config_.max_pump_iterations, true);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (admissions_.load() == admissions || shutting_down_.load()) {
      driver_active_ = false;
      break;
    }
  }
  if (!status.has_value() && status.error().code() != ErrorCode::cancelled) {
    log(LogLevel::error, "execution driver stopped", {field("error", status.error().to_string())});
  }
  idle_cv_.notify_all();
}

Status Engine::run_until_idle() {
  bool owns_driver = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!driver_active_) {
      driver_active_ = true;
      owns_driver = true;
    }
  }
  if (!owns_driver) {
    return wait_for_idle();
  }
  const Status status = run_steps_bounded(config_.max_pump_iterations, true);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    driver_active_ = false;
  }
  idle_cv_.notify_all();
  return status;
}

Status Engine::wait_for_idle() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_cv_.wait(lock, [this] { return !driver_active_ && !has_in_flight_locked(); });
  return ok_status();
}

bool Engine::is_idle() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return !driver_active_ && !has_in_flight_locked();
}

bool Engine::has_in_flight_locked() const {
  for (const auto& attempt : store_.attempts()) {
    if (attempt_state_is_in_flight(attempt.state)) {
      return true;
    }
  }
  return false;
}

Status Engine::shutdown() {
  if (shutting_down_.exchange(true)) {
    return ok_status();
  }
  if (pool_) {
    pool_->stop();
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  driver_active_ = false;
  idle_cv_.notify_all();
  return store_.close();
}

}  // namespace fum
