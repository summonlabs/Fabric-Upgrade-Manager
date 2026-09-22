// Engine: lifecycle, admission, queries and durable state.
#include "fum/engine/engine.hpp"

#include <algorithm>
#include <functional>

#include "fum/core/fs.hpp"
#include "fum/core/hash.hpp"
#include "fum/engine/planner.hpp"

namespace fum {

const char* executor_kind_name(ExecutorKind kind) noexcept {
  switch (kind) {
    case ExecutorKind::deterministic_inline: return "deterministic-inline";
    case ExecutorKind::thread_pool: return "thread-pool";
  }
  return "unknown";
}

Result<ExecutorKind> parse_executor_kind(std::string_view text) {
  if (text == "deterministic-inline") return ExecutorKind::deterministic_inline;
  if (text == "thread-pool") return ExecutorKind::thread_pool;
  return make_error(ErrorCode::invalid_argument, "unknown executor kind", std::string(text));
}

InventorySourcePort::~InventorySourcePort() = default;

const char* operation_kind_name(OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::prepare: return "prepare";
    case OperationKind::activate: return "activate";
    case OperationKind::verify: return "verify";
    case OperationKind::rollback: return "rollback";
  }
  return "unknown";
}

json::Value OperationOutcome::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("kind", json::Value::make_string(operation_kind_name(kind)));
  value.set("skipped_because_applied", json::Value::make_bool(skipped_because_applied));
  value.set("gate_refused", json::Value::make_bool(gate_refused));
  value.set("gate_reason", json::Value::make_string(gate_reason));
  switch (kind) {
    case OperationKind::prepare: value.set("prepare", prepare.to_json()); break;
    case OperationKind::activate: value.set("activate", activate.to_json()); break;
    case OperationKind::verify: value.set("verify", verify.to_json()); break;
    case OperationKind::rollback: value.set("rollback", rollback.to_json()); break;
  }
  if (has_health) {
    value.set("health", health.to_json());
  }
  return value;
}

json::Value CampaignStatus::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("campaign", campaign.to_json());
  json::Value attempts_json = json::Value::make_array();
  for (const auto& attempt : attempts) {
    attempts_json.push(attempt.to_json());
  }
  value.set("attempts", std::move(attempts_json));
  value.set("in_flight", json::Value::make_uint(in_flight));
  value.set("ticket_current", json::Value::make_bool(ticket_current));
  value.set("ticket_detail", json::Value::make_string(ticket_detail));
  value.set("skew", skew.to_json());
  json::Value decisions_json = json::Value::make_array();
  for (const auto& decision : decisions) {
    decisions_json.push(decision.to_json());
  }
  value.set("decisions", std::move(decisions_json));
  return value;
}

Engine::Engine(EngineConfig config, EngineDependencies dependencies)
    : config_(std::move(config)), deps_(std::move(dependencies)), policy_(config_.policy) {}

Engine::~Engine() {
  if (!shutting_down_.load()) {
    static_cast<void>(shutdown());
  }
}

void Engine::log(LogLevel level, std::string message, std::vector<LogField> fields) const {
  if (config_.logger != nullptr) {
    config_.logger->log(level, std::move(message), std::move(fields));
  }
}

Result<std::unique_ptr<Engine>> Engine::open(const EngineConfig& config,
                                             EngineDependencies dependencies) {
  FUM_TRYV(config.policy.validate());
  if (config.store.directory.empty()) {
    return make_error(ErrorCode::invalid_argument,
                      "an engine requires a state directory for durable state");
  }
  if (!dependencies.inventory) {
    return make_error(ErrorCode::not_integrated,
                      "an inventory source is required to resolve upgrade targets");
  }
  if (!dependencies.clock) {
    dependencies.clock = std::make_shared<SystemClock>();
  }
  if (config.worker_threads == 0) {
    return make_error(ErrorCode::invalid_argument, "worker_threads must be at least 1");
  }
  auto engine = std::unique_ptr<Engine>(new Engine(config, std::move(dependencies)));
  FUM_TRYV(engine->initialize());
  return engine;
}

Status Engine::initialize() {
  const Timestamp now = deps_.clock->now();
  FUM_TRY(store_, DurableStore::open(config_.store));
  incarnation_ = store_.incarnation();
  auto next_epoch = store_.epoch().try_next();
  if (!next_epoch.has_value()) {
    return next_epoch.error();
  }
  epoch_ = next_epoch.value();
  FUM_TRYV(store_.set_epoch(epoch_, now));
  FUM_TRYV(store_.put_policy(policy_, now));
  if (config_.executor == ExecutorKind::thread_pool) {
    pool_ = std::make_unique<WorkerPool>(
        WorkerPool::Options{config_.worker_threads, config_.max_queue});
    pool_mode_ = true;
  }
  FUM_TRYV(apply_reconciliation(now));
  return ok_status();
}

Status Engine::apply_reconciliation(Timestamp now) {
  const std::vector<CampaignRecord> campaign_list = store_.campaigns();
  const std::vector<AttemptRecord> attempt_list = store_.attempts();
  reconciliation_ = plan_reconciliation(campaign_list, attempt_list, incarnation_, now);

  for (const auto& action : reconciliation_.actions) {
    switch (action.kind) {
      case ReconcileActionKind::abandon_attempt: {
        const AttemptId id = make_AttemptId(action.subject);
        const AttemptRecord* existing = store_.attempt(id);
        if (existing == nullptr) {
          break;
        }
        AttemptRecord updated = *existing;
        updated.state = AttemptState::abandoned;
        auto revision = updated.revision.try_next();
        updated.revision = revision.has_value() ? revision.value() : updated.revision;
        updated.last_error = action.reason;
        updated.updated_at = now;
        FUM_TRYV(store_.put_attempt(updated, now));
        break;
      }
      case ReconcileActionKind::block_campaign: {
        const CampaignId id = make_CampaignId(action.subject);
        const CampaignRecord* existing = store_.campaign(id);
        if (existing == nullptr) {
          break;
        }
        CampaignRecord updated = *existing;
        TransitionGuards guards = guards_locked(updated);
        guards.failure_recorded = true;
        updated.block_reason = action.reason;
        updated.failure_reason = action.reason;
        FUM_TRYV(transition_locked(updated, UpgradeState::blocked, guards));
        break;
      }
      case ReconcileActionKind::invalidate_ticket: {
        const CampaignId id = make_CampaignId(action.subject);
        const CampaignRecord* existing = store_.campaign(id);
        if (existing == nullptr) {
          break;
        }
        CampaignRecord updated = *existing;
        updated.ticket = PreflightTicket{};
        auto revision = updated.revision.try_next();
        updated.revision = revision.has_value() ? revision.value() : updated.revision;
        updated.updated_at = now;
        FUM_TRYV(store_.put_campaign(updated, now));
        const Decision decision =
            make_decision_locked(DecisionKind::reconcile, updated, DecisionOutcome::deny,
                                 "invalidate preflight ticket", action.reason);
        record_decision_locked(decision);
        break;
      }
      case ReconcileActionKind::note:
        break;
    }
  }
  if (!reconciliation_.actions.empty()) {
    log(LogLevel::warn, "restart reconciliation applied",
        {field("abandoned_attempts", static_cast<std::uint64_t>(reconciliation_.abandoned_attempts)),
         field("blocked_campaigns", static_cast<std::uint64_t>(reconciliation_.blocked_campaigns)),
         field("invalidated_tickets",
               static_cast<std::uint64_t>(reconciliation_.invalidated_tickets))});
  }
  return ok_status();
}

Status Engine::refresh_inventory(Timestamp now, bool force) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!force) {
      const FreshnessVerdict verdict =
          evaluate_freshness(last_inventory_.observed_at, last_inventory_.validity,
                             last_inventory_.incarnation, now, incarnation_);
      if (verdict.fresh) {
        return ok_status();
      }
    }
  }
  auto resolved = deps_.inventory->resolve(now, incarnation_);
  if (!resolved.has_value()) {
    return resolved.error();
  }
  InventorySnapshot snapshot;
  snapshot.targets = std::move(resolved).value();
  std::sort(snapshot.targets.begin(), snapshot.targets.end(),
            [](const TargetDescriptor& a, const TargetDescriptor& b) { return a.id < b.id; });
  const std::lock_guard<std::mutex> lock(mutex_);
  auto revision = last_inventory_.revision.try_next();
  snapshot.revision = revision.has_value() ? revision.value() : Revision(1);
  snapshot.incarnation = incarnation_;
  snapshot.observed_at = now;
  snapshot.validity = policy_.inventory_validity;
  for (const auto& target : snapshot.targets) {
    FUM_TRYV(target.validate());
  }
  last_inventory_ = std::move(snapshot);
  return ok_status();
}

Result<InventorySnapshot> Engine::inventory_locked() const {
  if (last_inventory_.targets.empty() && last_inventory_.observed_at.unix_nanos() == 0) {
    return make_error(ErrorCode::stale, "no inventory has been resolved yet");
  }
  return last_inventory_;
}

Result<IntegrityAssessment> Engine::resolve_artifact(const ArtifactDescriptor& artifact,
                                                     std::string& payload_path) {
  if (!deps_.artifacts) {
    return make_error(ErrorCode::not_integrated,
                      "artifact source is not integrated; artifact bytes cannot be resolved");
  }
  auto payload = deps_.artifacts->resolve(artifact);
  if (!payload.has_value()) {
    return payload.error();
  }
  if (!payload.value().present) {
    return make_error(ErrorCode::not_found, "artifact payload is not present",
                      payload.value().detail);
  }
  payload_path = payload.value().path;
  const IntegrityAssessment assessment = verify_artifact_file(artifact, payload.value().path);
  const std::lock_guard<std::mutex> lock(mutex_);
  last_integrity_ = assessment;
  artifact_payload_path_ = payload.value().path;
  return assessment;
}

Result<std::map<std::string, CompatibilityVerdict>> Engine::query_compatibility(
    const UpgradePlan& plan, const InventorySnapshot& inventory, Timestamp now) {
  std::map<std::string, CompatibilityVerdict> verdicts;
  if (!deps_.compatibility) {
    return verdicts;
  }
  for (const auto& stage : plan.stages) {
    for (const auto& target_id : stage.targets) {
      if (verdicts.find(target_id.str()) != verdicts.end()) {
        continue;
      }
      const TargetDescriptor* target = inventory.find(target_id);
      if (target == nullptr) {
        continue;
      }
      CompatibilityQuery query;
      query.component = plan.component;
      query.from_version = target->installed_version;
      query.to_version = plan.artifact.version();
      query.to_build = plan.artifact.build();
      query.platform = target->platform;
      query.target_capabilities = target->capabilities;
      query.strategy = plan.strategy;
      query.incarnation = incarnation_;
      query.at = now;
      auto verdict = deps_.compatibility->query(query);
      if (!verdict.has_value()) {
        CompatibilityVerdict failed;
        failed.answered = false;
        failed.detail = verdict.error().to_string();
        verdicts[target_id.str()] = failed;
        continue;
      }
      verdicts[target_id.str()] = std::move(verdict).value();
    }
  }
  return verdicts;
}

Result<UpgradePlan> Engine::plan(const UpgradePlanRequest& request) {
  const Timestamp now = deps_.clock->now();
  FUM_TRYV(refresh_inventory(now, false));
  InventorySnapshot snapshot;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot = last_inventory_;
  }
  PlannerInput input;
  input.request = request;
  input.inventory = std::move(snapshot);
  input.policy = &policy_;
  input.now = now;
  return build_plan(input);
}

Result<UpgradePlan> Engine::create_campaign(const UpgradePlanRequest& request) {
  const Timestamp now = deps_.clock->now();
  UpgradePlan built;
  FUM_TRY(built, plan(request));
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (store_.campaign(built.campaign) != nullptr) {
      return make_error(ErrorCode::already_exists, "a campaign with this identity already exists",
                        built.campaign.str());
    }
  }
  CampaignRecord record;
  record.id = built.campaign;
  record.generation = Generation(1);
  record.epoch = epoch_;
  record.authority = request.authority;
  record.component = request.component;
  record.state = UpgradeState::proposed;
  record.strategy = request.strategy;
  record.artifact = request.artifact;
  record.plan = built;
  record.revision = Revision(1);
  record.owner_incarnation = incarnation_;
  record.created_at = now;
  record.updated_at = now;
  record.irreversible_acknowledged = request.acknowledge_irreversible_steps;
  record.acknowledged_steps = request.acknowledged_irreversible_steps;
  record.rollback_eligible = request.artifact.rollback().reversible;
  for (std::size_t i = 0; i < built.stages.size(); ++i) {
    StageProgress progress;
    progress.id = built.stages[i].id;
    progress.state = StageState::pending;
    record.stages.push_back(std::move(progress));
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    FUM_TRYV(store_.put_campaign(record, now));
    const Decision decision = make_decision_locked(
        DecisionKind::plan, record,
        built.executable() ? DecisionOutcome::allow : DecisionOutcome::deny,
        built.executable() ? "campaign created with an executable plan"
                           : "campaign created with a rejected plan",
        built.executable()
            ? "the plan passed every planning gate"
            : "the plan carries " + std::to_string(built.rejections.size()) + " rejection(s)",
        {DecisionInput{"strategy", strategy_kind_name(built.strategy)},
         DecisionInput{"stages", std::to_string(built.stages.size())},
         DecisionInput{"targets", std::to_string(built.target_count())},
         DecisionInput{"plan_fingerprint", built.fingerprint()}});
    record_decision_locked(decision);
  }
  log(LogLevel::info, "campaign created",
      {field("campaign", record.id.str()), field("state", upgrade_state_name(record.state)),
       field("executable", built.executable())});
  return built;
}

Result<PreflightReport> Engine::preflight(const CampaignId& campaign_id) {
  const Timestamp now = deps_.clock->now();
  CampaignRecord campaign;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const CampaignRecord* existing = store_.campaign(campaign_id);
    if (existing == nullptr) {
      return make_error(ErrorCode::not_found, "unknown campaign", campaign_id.str());
    }
    campaign = *existing;
  }
  FUM_TRYV(refresh_inventory(now, true));
  InventorySnapshot snapshot;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot = last_inventory_;
  }

  PreflightInput input;
  input.campaign = &campaign;
  input.plan = &campaign.plan;
  input.policy = &policy_;
  input.inventory = snapshot;
  input.now = now;
  input.incarnation = incarnation_;
  input.epoch = epoch_;
  input.registry_integrated = static_cast<bool>(deps_.compatibility);
  input.drain_integrated = static_cast<bool>(deps_.drain);
  input.artifact_source_integrated = static_cast<bool>(deps_.artifacts);

  std::string payload_path;
  auto integrity = resolve_artifact(campaign.artifact, payload_path);
  if (integrity.has_value()) {
    input.integrity = integrity.value();
    input.payload.path = payload_path;
    input.payload.bytes = integrity.value().bytes;
    input.payload.present = true;
  } else {
    input.integrity.detail = integrity.error().to_string();
    input.payload.present = false;
    input.payload.detail = integrity.error().to_string();
  }
  auto verdicts = query_compatibility(campaign.plan, snapshot, now);
  if (verdicts.has_value()) {
    input.verdicts = std::move(verdicts).value();
  }
  const PreflightReport report = run_preflight(input);

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const CampaignRecord* existing = store_.campaign(campaign_id);
    if (existing == nullptr) {
      return make_error(ErrorCode::not_found, "campaign disappeared during preflight",
                        campaign_id.str());
    }
    CampaignRecord updated = *existing;
    updated.last_skew = report.skew;
    if (report.passed) {
      updated.ticket = report.ticket();
      TransitionGuards guards = guards_locked(updated);
      guards.preflight_current = true;
      guards.artifact_integrity_verified = report.integrity.verified;
      auto revision = updated.revision.try_next();
      updated.revision = revision.has_value() ? revision.value() : updated.revision;
      updated.updated_at = now;
      FUM_TRYV(store_.put_campaign(updated, now));
      FUM_TRYV(store_.put_ticket(report.ticket(), now));
      if (updated.state == UpgradeState::proposed || updated.state == UpgradeState::blocked ||
          updated.state == UpgradeState::paused) {
        FUM_TRYV(transition_locked(updated, UpgradeState::validated, guards));
      }
    } else {
      updated.ticket = PreflightTicket{};
      auto revision = updated.revision.try_next();
      updated.revision = revision.has_value() ? revision.value() : updated.revision;
      updated.updated_at = now;
      updated.block_reason = report.blockers.empty() ? "preflight failed" : report.blockers.front();
      FUM_TRYV(store_.put_campaign(updated, now));
      if (updated.state == UpgradeState::proposed) {
        TransitionGuards guards = guards_locked(updated);
        guards.failure_recorded = true;
        FUM_TRYV(transition_locked(updated, UpgradeState::blocked, guards));
      }
    }
    std::vector<DecisionInput> inputs;
    inputs.push_back(DecisionInput{"blockers", std::to_string(report.blockers.size())});
    inputs.push_back(DecisionInput{"targets", std::to_string(report.targets.size())});
    inputs.push_back(DecisionInput{"artifact_digest", campaign.artifact.digest().hex()});
    inputs.push_back(DecisionInput{"inventory_revision", std::to_string(snapshot.revision.value())});
    std::vector<DecisionEvidenceRef> evidence;
    evidence.push_back(DecisionEvidenceRef{make_EvidenceId("inventory/" + std::to_string(snapshot.revision.value())),
                                           "inventory-probe", "inventory revision " +
                                                                  std::to_string(snapshot.revision.value())});
    evidence.push_back(DecisionEvidenceRef{make_EvidenceId("integrity/" + campaign.artifact.digest().hex().substr(0, 16)),
                                           "artifact-integrity", report.integrity.detail});
    const Decision decision = make_decision_locked(
        DecisionKind::preflight, updated,
        report.passed ? DecisionOutcome::allow : DecisionOutcome::deny,
        report.passed ? "admit the campaign for execution" : "refuse execution",
        report.passed ? "every preflight gate passed"
                      : "preflight blockers: " + (report.blockers.empty()
                                                      ? std::string("unspecified")
                                                      : report.blockers.front()),
        std::move(inputs), {}, std::move(evidence));
    record_decision_locked(decision);
  }
  log(report.passed ? LogLevel::info : LogLevel::warn, "preflight completed",
      {field("campaign", campaign_id.str()), field("passed", report.passed),
       field("blockers", static_cast<std::uint64_t>(report.blockers.size()))});
  return report;
}

}  // namespace fum
