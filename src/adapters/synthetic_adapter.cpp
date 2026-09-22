#include "fum/adapters/synthetic_adapter.hpp"

#include <algorithm>

#include "fum/core/fs.hpp"

namespace fum {

const char* synthetic_fault_name(SyntheticFault fault) noexcept {
  switch (fault) {
    case SyntheticFault::none: return "none";
    case SyntheticFault::prepare_failure: return "prepare-failure";
    case SyntheticFault::prepare_timeout: return "prepare-timeout";
    case SyntheticFault::activate_failure: return "activate-failure";
    case SyntheticFault::activate_timeout_effect: return "activate-timeout-effect";
    case SyntheticFault::activate_timeout_noop: return "activate-timeout-noop";
    case SyntheticFault::activate_wrong_version: return "activate-wrong-version";
    case SyntheticFault::verify_failure: return "verify-failure";
    case SyntheticFault::verify_unhealthy: return "verify-unhealthy";
    case SyntheticFault::verify_stale: return "verify-stale";
    case SyntheticFault::verify_foreign_incarnation: return "verify-foreign-incarnation";
    case SyntheticFault::rollback_failure: return "rollback-failure";
    case SyntheticFault::observe_failure: return "observe-failure";
    case SyntheticFault::crash_target: return "crash-target";
  }
  return "unknown";
}

void SyntheticGate::open() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
  }
  cv_.notify_all();
}

void SyntheticGate::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return open_; });
}

void SyntheticGate::mark_reached() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    reached_ = true;
  }
  reached_cv_.notify_all();
}

void SyntheticGate::wait_until_reached() {
  std::unique_lock<std::mutex> lock(mutex_);
  reached_cv_.wait(lock, [this] { return reached_; });
}

bool SyntheticGate::opened() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return open_;
}

bool SyntheticGate::reached() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return reached_;
}

SyntheticFaultScope synthetic_fault_scope(SyntheticFault fault) noexcept {
  switch (fault) {
    case SyntheticFault::prepare_failure:
    case SyntheticFault::prepare_timeout:
      return SyntheticFaultScope::prepare;
    case SyntheticFault::activate_failure:
    case SyntheticFault::activate_timeout_effect:
    case SyntheticFault::activate_timeout_noop:
    case SyntheticFault::activate_wrong_version:
    case SyntheticFault::crash_target:
      return SyntheticFaultScope::activate;
    case SyntheticFault::verify_failure:
    case SyntheticFault::verify_unhealthy:
    case SyntheticFault::verify_stale:
    case SyntheticFault::verify_foreign_incarnation:
      return SyntheticFaultScope::verify;
    case SyntheticFault::rollback_failure:
      return SyntheticFaultScope::rollback;
    case SyntheticFault::observe_failure:
    case SyntheticFault::none:
      return SyntheticFaultScope::observe;
  }
  return SyntheticFaultScope::observe;
}

SyntheticAdapter::SyntheticAdapter(AdapterId id, AdapterKind kind)
    : id_(std::move(id)), kind_(kind) {
  claims_.strategies = {StrategyKind::in_place, StrategyKind::restart_based,
                        StrategyKind::redundant_pair_rolling};
  claims_.supports_prepare = true;
  claims_.supports_rollback = true;
  claims_.supports_health_probe = true;
  claims_.supports_version_observation = true;
  claims_.max_parallel_activations = 1;
}

void SyntheticAdapter::set_claims(AdapterClaims claims) { claims_ = std::move(claims); }

void SyntheticAdapter::add_target(SyntheticTarget target) {
  const std::lock_guard<std::mutex> lock(mutex_);
  State state;
  state.target = std::move(target);
  if (state.target.authority.empty()) {
    state.target.authority = make_AuthorityId("synthetic-authority");
  }
  targets_[state.target.id.str()] = std::move(state);
}

void SyntheticAdapter::remove_target(const TargetId& target) {
  const std::lock_guard<std::mutex> lock(mutex_);
  targets_.erase(target.str());
}

void SyntheticAdapter::enqueue_fault(const TargetId& target, SyntheticFault fault) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = targets_.find(target.str());
  if (it != targets_.end()) {
    it->second.faults.push_back(fault);
  }
}

void SyntheticAdapter::set_persistent_fault(const TargetId& target, SyntheticFault fault) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = targets_.find(target.str());
  if (it != targets_.end()) {
    it->second.persistent = fault;
  }
}

void SyntheticAdapter::clear_faults(const TargetId& target) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = targets_.find(target.str());
  if (it != targets_.end()) {
    it->second.faults.clear();
    it->second.persistent = SyntheticFault::none;
  }
}

void SyntheticAdapter::set_health(const TargetId& target, bool healthy, std::string detail) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = targets_.find(target.str());
  if (it != targets_.end()) {
    it->second.target.healthy = healthy;
    it->second.target.health_detail = std::move(detail);
  }
}

void SyntheticAdapter::set_health_validity(const TargetId& target, Duration validity) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = targets_.find(target.str());
  if (it != targets_.end()) {
    it->second.target.health_validity = validity;
  }
}

void SyntheticAdapter::set_health_age_offset(const TargetId& target, Duration offset) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = targets_.find(target.str());
  if (it != targets_.end()) {
    it->second.target.health_age_offset = offset;
  }
}

void SyntheticAdapter::set_process_alive(const TargetId& target, bool alive) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = targets_.find(target.str());
  if (it != targets_.end()) {
    it->second.target.process_alive = alive;
  }
}

void SyntheticAdapter::set_version(const TargetId& target, Version version, BuildId build) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = targets_.find(target.str());
  if (it != targets_.end()) {
    it->second.target.version = std::move(version);
    it->second.target.build = std::move(build);
  }
}

void SyntheticAdapter::gate_next_activate(const TargetId& target,
                                         std::shared_ptr<SyntheticGate> gate) {
  const std::lock_guard<std::mutex> lock(mutex_);
  static_cast<void>(target);
  activate_gates_.push_back(std::move(gate));
}

void SyntheticAdapter::gate_next_verify(const TargetId& target, std::shared_ptr<SyntheticGate> gate) {
  const std::lock_guard<std::mutex> lock(mutex_);
  static_cast<void>(target);
  verify_gates_.push_back(std::move(gate));
}

std::shared_ptr<SyntheticGate> SyntheticAdapter::take_gate(const TargetId& target, bool activate) {
  const std::lock_guard<std::mutex> lock(mutex_);
  static_cast<void>(target);
  auto& queue = activate ? activate_gates_ : verify_gates_;
  if (queue.empty()) {
    return nullptr;
  }
  auto gate = queue.front();
  queue.pop_front();
  return gate;
}

Result<SyntheticAdapter::State*> SyntheticAdapter::find(const TargetId& target) {
  const auto it = targets_.find(target.str());
  if (it == targets_.end()) {
    return make_error(ErrorCode::not_found, "the synthetic adapter does not know this target",
                      target.str());
  }
  return &it->second;
}

SyntheticFault SyntheticAdapter::next_fault(State& state, SyntheticFaultScope scope) {
  for (auto it = state.faults.begin(); it != state.faults.end(); ++it) {
    if (synthetic_fault_scope(*it) == scope) {
      const SyntheticFault fault = *it;
      state.faults.erase(it);
      ++counters_.faults_injected;
      return fault;
    }
  }
  if (state.persistent != SyntheticFault::none &&
      synthetic_fault_scope(state.persistent) == scope) {
    ++counters_.faults_injected;
    return state.persistent;
  }
  return SyntheticFault::none;
}

bool SyntheticAdapter::already_done(State& state, const std::string& key) {
  if (std::find(state.completed_operations.begin(), state.completed_operations.end(), key) !=
      state.completed_operations.end()) {
    ++counters_.duplicate_operations;
    return true;
  }
  state.completed_operations.push_back(key);
  if (state.completed_operations.size() > 128) {
    state.completed_operations.erase(state.completed_operations.begin());
  }
  return false;
}

Result<TargetObservation> SyntheticAdapter::observe(const TargetId& target, Timestamp now,
                                                    Incarnation incarnation) {
  const std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.observe_calls;
  auto found = find(target);
  if (!found.has_value()) {
    return found.error();
  }
  State& state = *found.value();
  const SyntheticFault fault = next_fault(state, SyntheticFaultScope::observe);
  if (fault == SyntheticFault::observe_failure) {
    return make_error(ErrorCode::timeout, "the synthetic target did not answer the observation");
  }
  TargetObservation observation;
  observation.target = target;
  observation.version = state.target.version;
  observation.build = state.target.build;
  observation.healthy = state.target.healthy && state.target.process_alive;
  observation.health_detail = state.target.process_alive ? state.target.health_detail
                                                         : "the synthetic target process is down";
  observation.observed_at = now.plus(Duration::from_nanos(-state.target.health_age_offset.nanos()));
  observation.incarnation = incarnation;
  observation.source = id_.str();
  return observation;
}

Result<PrepareOutcome> SyntheticAdapter::prepare(const TargetOperation& operation) {
  const std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.prepare_calls;
  operation_log_.push_back("prepare " + operation.key.to_string());
  auto found = find(operation.target.id);
  if (!found.has_value()) {
    return found.error();
  }
  State& state = *found.value();
  const SyntheticFault fault = next_fault(state, SyntheticFaultScope::prepare);
  if (fault == SyntheticFault::prepare_failure) {
    return make_error(ErrorCode::io_error, "the synthetic target refused the staged artifact");
  }
  if (fault == SyntheticFault::prepare_timeout) {
    return make_error(ErrorCode::timeout, "the synthetic target lost the prepare acknowledgement");
  }
  PrepareOutcome outcome;
  const bool duplicate = already_done(state, "prepare:" + operation.key.to_string());
  state.staged_version = operation.artifact.version();
  state.staged_build = operation.artifact.build();
  outcome.prepared = true;
  outcome.already_prepared = duplicate;
  outcome.detail = duplicate ? "the artifact was already staged (idempotent repeat)"
                             : "staged " + operation.artifact.version().text();
  outcome.steps.push_back("write-payload");
  outcome.steps.push_back("verify-digest");
  return outcome;
}

Result<ActivateOutcome> SyntheticAdapter::activate(const TargetOperation& operation) {
  // The gate is waited on outside the adapter lock so other targets keep
  // making progress while one operation is held open.
  if (const auto gate = take_gate(operation.target.id, true); gate != nullptr) {
    gate->mark_reached();
    gate->wait();
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.activate_calls;
  operation_log_.push_back("activate " + operation.key.to_string());
  auto found = find(operation.target.id);
  if (!found.has_value()) {
    return found.error();
  }
  State& state = *found.value();
  const SyntheticFault fault = next_fault(state, SyntheticFaultScope::activate);
  switch (fault) {
    case SyntheticFault::activate_failure:
      return make_error(ErrorCode::io_error, "the synthetic target refused the activation");
    case SyntheticFault::activate_timeout_noop:
      return make_error(ErrorCode::timeout,
                        "the synthetic target lost the activation acknowledgement");
    case SyntheticFault::activate_timeout_effect: {
      state.previous_version = state.target.version;
      state.previous_build = state.target.build;
      state.target.version = operation.artifact.version();
      state.target.build = operation.artifact.build();
      return make_error(ErrorCode::timeout,
                        "the synthetic target applied the activation but the acknowledgement was "
                        "lost");
    }
    case SyntheticFault::crash_target: {
      state.previous_version = state.target.version;
      state.previous_build = state.target.build;
      state.target.version = operation.artifact.version();
      state.target.build = operation.artifact.build();
      state.target.process_alive = false;
      state.target.healthy = false;
      state.target.health_detail = "the synthetic target crashed during activation";
      return make_error(ErrorCode::timeout,
                        "the synthetic target crashed after applying the activation");
    }
    case SyntheticFault::activate_wrong_version: {
      ActivateOutcome outcome;
      outcome.activated = true;
      outcome.observed_version = state.target.version;
      outcome.observed_build = state.target.build;
      outcome.detail = "the installer exited successfully but the running version did not change";
      return outcome;
    }
    default:
      break;
  }
  ActivateOutcome outcome;
  const bool duplicate = already_done(state, "activate:" + operation.key.to_string());
  state.previous_version = state.target.version;
  state.previous_build = state.target.build;
  state.target.version = operation.artifact.version();
  state.target.build = operation.artifact.build();
  state.target.process_alive = true;
  outcome.activated = true;
  outcome.already_active = duplicate;
  outcome.restart_performed = claims_.supports_health_probe;
  outcome.observed_version = state.target.version;
  outcome.observed_build = state.target.build;
  outcome.detail = duplicate ? "the activation was already applied (idempotent repeat)"
                             : "activated " + operation.artifact.version().text();
  return outcome;
}

Result<VerifyOutcome> SyntheticAdapter::verify(const TargetOperation& operation) {
  if (const auto gate = take_gate(operation.target.id, false); gate != nullptr) {
    gate->mark_reached();
    gate->wait();
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.verify_calls;
  operation_log_.push_back("verify " + operation.key.to_string());
  auto found = find(operation.target.id);
  if (!found.has_value()) {
    return found.error();
  }
  State& state = *found.value();
  const SyntheticFault fault = next_fault(state, SyntheticFaultScope::verify);
  VerifyOutcome outcome;
  outcome.observed_version = state.target.version;
  outcome.observed_build = state.target.build;
  outcome.healthy = state.target.healthy && state.target.process_alive;
  outcome.observed_at = operation.now.plus(
      Duration::from_nanos(-state.target.health_age_offset.nanos()));
  outcome.validity = state.target.health_validity;
  // The observation belongs to the process incarnation that asked for it.
  outcome.incarnation = operation.fence.incarnation;
  outcome.checks.push_back("version");
  outcome.checks.push_back("health");
  switch (fault) {
    case SyntheticFault::verify_failure:
      outcome.verified = false;
      outcome.detail = "the synthetic verification rejected the target";
      return outcome;
    case SyntheticFault::verify_unhealthy:
      outcome.verified = true;
      outcome.healthy = false;
      outcome.detail = "the synthetic target reports unhealthy after activation";
      return outcome;
    case SyntheticFault::verify_stale:
      outcome.verified = true;
      outcome.observed_at = operation.now.plus(Duration::from_nanos(-state.target.health_validity.nanos() * 4));
      outcome.detail = "the verification evidence is older than its validity window";
      return outcome;
    case SyntheticFault::verify_foreign_incarnation:
      outcome.verified = true;
      outcome.incarnation = Incarnation(outcome.incarnation.value() == 0
                                            ? 1
                                            : outcome.incarnation.value() - 1);
      outcome.detail = "the verification evidence belongs to a previous incarnation";
      return outcome;
    default:
      break;
  }
  outcome.verified = true;
  outcome.detail = "the synthetic target runs " + state.target.version.text() + " and is healthy";
  return outcome;
}

Result<RollbackOutcome> SyntheticAdapter::rollback(const TargetOperation& operation) {
  const std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.rollback_calls;
  operation_log_.push_back("rollback " + operation.key.to_string());
  auto found = find(operation.target.id);
  if (!found.has_value()) {
    return found.error();
  }
  State& state = *found.value();
  const SyntheticFault fault = next_fault(state, SyntheticFaultScope::rollback);
  if (fault == SyntheticFault::rollback_failure) {
    return make_error(ErrorCode::io_error, "the synthetic target refused the rollback");
  }
  RollbackOutcome outcome;
  const bool duplicate = already_done(state, "rollback:" + operation.key.to_string());
  if (!state.previous_version.empty()) {
    state.target.version = state.previous_version;
    state.target.build = state.previous_build;
  }
  state.target.process_alive = true;
  outcome.rolled_back = true;
  outcome.already_rolled_back = duplicate;
  outcome.observed_version = state.target.version;
  outcome.observed_build = state.target.build;
  outcome.detail = duplicate ? "the rollback was already applied (idempotent repeat)"
                             : "restored " + state.target.version.text();
  return outcome;
}

Result<bool> SyntheticAdapter::process_alive(const TargetId& target) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto found = find(target);
  if (!found.has_value()) {
    return found.error();
  }
  return found.value()->target.process_alive;
}

SyntheticAdapter::Counters SyntheticAdapter::counters() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return counters_;
}

std::vector<std::string> SyntheticAdapter::operation_log() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return operation_log_;
}

Version SyntheticAdapter::version_of(const TargetId& target) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = targets_.find(target.str());
  return it == targets_.end() ? Version{} : it->second.target.version;
}

// ---------------------------------------------------------------------------
// Fabric stand-ins
// ---------------------------------------------------------------------------
SyntheticCompatibilityRegistry::SyntheticCompatibilityRegistry(std::string revision)
    : revision_(std::move(revision)) {}

void SyntheticCompatibilityRegistry::add_entry(CompatibilityMatrixEntry entry) {
  const std::lock_guard<std::mutex> lock(mutex_);
  entries_.push_back(std::move(entry));
}

void SyntheticCompatibilityRegistry::set_revision(std::string revision) {
  const std::lock_guard<std::mutex> lock(mutex_);
  revision_ = std::move(revision);
}

std::size_t SyntheticCompatibilityRegistry::queries() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return queries_;
}

Result<CompatibilityVerdict> SyntheticCompatibilityRegistry::query(
    const CompatibilityQuery& request) {
  const std::lock_guard<std::mutex> lock(mutex_);
  ++queries_;
  if (has_failure_) {
    return make_error(failure_, "the synthetic compatibility registry refused the query");
  }
  CompatibilityVerdict verdict;
  verdict.answered = answered_;
  verdict.registry_revision = revision_;
  verdict.registry_policy = "synthetic-compatibility-policy";
  verdict.validity = Duration::from_seconds(300);
  if (!answered_) {
    verdict.detail = "the synthetic registry declines to answer";
    return verdict;
  }
  verdict.evaluated_constraints.push_back("component=" + request.component.str());
  verdict.evaluated_constraints.push_back("from=" + request.from_version.text());
  verdict.evaluated_constraints.push_back("to=" + request.to_version.text());
  verdict.evaluated_constraints.push_back("platform=" + request.platform);
  verdict.compatible = default_compatible_;
  bool matched = false;
  for (const auto& entry : entries_) {
    if (entry.component == request.component && entry.from == request.from_version &&
        entry.to == request.to_version) {
      verdict.compatible = entry.compatible;
      verdict.rollback_supported = entry.rollback_supported;
      verdict.rollback_boundary = entry.rollback_boundary;
      verdict.rollback_artifact_known = entry.rollback_supported;
      if (!entry.compatible) {
        verdict.blockers.push_back(entry.reason.empty() ? "declared incompatible by the registry"
                                                        : entry.reason);
      }
      matched = true;
      break;
    }
  }
  if (!matched && !default_compatible_) {
    verdict.blockers.push_back("no compatibility entry covers " + request.from_version.text() +
                               " -> " + request.to_version.text());
  }
  verdict.detail = verdict.compatible ? "compatible" : "incompatible";
  return verdict;
}

void SyntheticInventory::set_targets(std::vector<TargetDescriptor> targets) {
  const std::lock_guard<std::mutex> lock(mutex_);
  targets_ = std::move(targets);
}

void SyntheticInventory::add_target(TargetDescriptor target) {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (auto& existing : targets_) {
    if (existing.id == target.id) {
      existing = std::move(target);
      return;
    }
  }
  targets_.push_back(std::move(target));
}

void SyntheticInventory::set_resolve_failure(ErrorCode code) {
  const std::lock_guard<std::mutex> lock(mutex_);
  failure_ = code;
  has_failure_ = code != ErrorCode::ok;
}

std::size_t SyntheticInventory::resolves() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return resolves_;
}

Result<std::vector<TargetDescriptor>> SyntheticInventory::resolve(Timestamp now,
                                                                 Incarnation incarnation) {
  const std::lock_guard<std::mutex> lock(mutex_);
  ++resolves_;
  static_cast<void>(now);
  static_cast<void>(incarnation);
  if (has_failure_) {
    return make_error(failure_, "the synthetic inventory source failed");
  }
  if (fail_next_) {
    fail_next_ = false;
    return make_error(ErrorCode::io_error, "the synthetic inventory source failed once");
  }
  return targets_;
}

SyntheticArtifactSource::SyntheticArtifactSource() {
  auto directory = fs::TempDir::create("fum-artifacts");
  if (directory.has_value()) {
    fs::TempDir kept = std::move(directory).value();
    kept.keep();
    directory_ = kept.path();
  } else {
    directory_ = "fum-artifacts-fallback";
    static_cast<void>(fs::ensure_directory(directory_));
  }
}

SyntheticArtifactSource::~SyntheticArtifactSource() {
  if (!directory_.empty()) {
    static_cast<void>(fs::remove_tree(directory_));
  }
}

Status SyntheticArtifactSource::publish(const ArtifactDescriptor& artifact, std::string_view payload,
                                        bool corrupt) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (directory_.empty()) {
    return make_error(ErrorCode::io_error, "the synthetic artifact source has no directory");
  }
  const std::string path = fs::join(directory_, artifact.id().str() + ".bin");
  std::string bytes(payload);
  if (corrupt) {
    bytes.append("-corrupted");
  }
  FUM_TRYV(fs::write_file(path, bytes));
  paths_[artifact.id().str()] = path;
  missing_.erase(artifact.id().str());
  return ok_status();
}

void SyntheticArtifactSource::set_missing(const ArtifactId& artifact) {
  const std::lock_guard<std::mutex> lock(mutex_);
  missing_[artifact.str()] = true;
}

void SyntheticArtifactSource::set_resolve_failure(ErrorCode code) {
  const std::lock_guard<std::mutex> lock(mutex_);
  failure_ = code;
  has_failure_ = code != ErrorCode::ok;
}

Result<ArtifactPayload> SyntheticArtifactSource::resolve(const ArtifactDescriptor& artifact) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (has_failure_) {
    return make_error(failure_, "the synthetic artifact source failed");
  }
  ArtifactPayload payload;
  const auto it = paths_.find(artifact.id().str());
  if (it == paths_.end() || missing_.find(artifact.id().str()) != missing_.end()) {
    payload.present = false;
    payload.detail = "the synthetic artifact source has no bytes for " + artifact.id().str();
    return payload;
  }
  payload.path = it->second;
  auto size = fs::file_size(it->second);
  payload.bytes = size.has_value() ? size.value() : 0;
  payload.present = true;
  payload.detail = "resolved";
  return payload;
}

void SyntheticHealthProbe::set_health(const TargetId& target, bool healthy, std::string detail) {
  const std::lock_guard<std::mutex> lock(mutex_);
  entries_[target.str()].healthy = healthy;
  entries_[target.str()].detail = std::move(detail);
}

void SyntheticHealthProbe::set_validity(const TargetId& target, Duration validity) {
  const std::lock_guard<std::mutex> lock(mutex_);
  entries_[target.str()].validity = validity;
}

void SyntheticHealthProbe::set_age_offset(const TargetId& target, Duration offset) {
  const std::lock_guard<std::mutex> lock(mutex_);
  entries_[target.str()].age_offset = offset;
}

void SyntheticHealthProbe::set_foreign_incarnation(const TargetId& target, bool value) {
  const std::lock_guard<std::mutex> lock(mutex_);
  entries_[target.str()].foreign_incarnation = value;
}

void SyntheticHealthProbe::set_probe_failure(ErrorCode code) {
  const std::lock_guard<std::mutex> lock(mutex_);
  failure_ = code;
  has_failure_ = code != ErrorCode::ok;
}

void SyntheticHealthProbe::clear() {
  const std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

std::size_t SyntheticHealthProbe::probes() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return probes_;
}

Result<HealthReport> SyntheticHealthProbe::probe(const TargetId& target, Timestamp now,
                                                 Incarnation incarnation) {
  const std::lock_guard<std::mutex> lock(mutex_);
  ++probes_;
  if (has_failure_) {
    return make_error(failure_, "the synthetic health probe failed");
  }
  Entry entry;
  const auto it = entries_.find(target.str());
  if (it != entries_.end()) {
    entry = it->second;
  }
  HealthReport report;
  report.healthy = entry.healthy;
  report.detail = entry.detail;
  report.observed_at = now.plus(Duration::from_nanos(-entry.age_offset.nanos()));
  report.validity = entry.validity;
  report.incarnation = entry.foreign_incarnation
                           ? Incarnation(incarnation.value() == 0 ? 1 : incarnation.value() - 1)
                           : incarnation;
  report.checks.push_back("synthetic-liveness");
  report.checks.push_back("synthetic-readiness");
  return report;
}

void SyntheticDrainFabric::set_refuse(bool refuse, std::string detail) {
  const std::lock_guard<std::mutex> lock(mutex_);
  refuse_ = refuse;
  detail_ = std::move(detail);
}

std::size_t SyntheticDrainFabric::acquisitions() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return acquisitions_;
}

std::size_t SyntheticDrainFabric::releases() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return releases_;
}

Result<DrainTicket> SyntheticDrainFabric::acquire(const DrainRequest& request) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (refuse_) {
    return make_error(ErrorCode::policy_denied, "the synthetic drain fabric refused", detail_);
  }
  ++acquisitions_;
  ++counter_;
  DrainTicket ticket;
  ticket.ticket = "drain-" + std::to_string(counter_);
  ticket.target = request.target;
  ticket.acquired_at = request.at;
  ticket.expires_at = request.at.plus(Duration::from_seconds(300));
  ticket.validity = Duration::from_seconds(300);
  ticket.valid = true;
  ticket.detail = "drained for upgrade";
  return ticket;
}

Status SyntheticDrainFabric::release(const DrainTicket& ticket) {
  const std::lock_guard<std::mutex> lock(mutex_);
  static_cast<void>(ticket);
  ++releases_;
  return ok_status();
}

Result<bool> SyntheticDrainFabric::still_drained(const DrainTicket& ticket) {
  const std::lock_guard<std::mutex> lock(mutex_);
  static_cast<void>(ticket);
  return still_drained_;
}

void SyntheticRolloutFabric::set_admit(bool admit, std::string detail) {
  const std::lock_guard<std::mutex> lock(mutex_);
  admit_ = admit;
  detail_ = std::move(detail);
}

std::size_t SyntheticRolloutFabric::admissions() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return admissions_;
}

Result<RolloutAdmission> SyntheticRolloutFabric::admit(const RolloutRequest& request) {
  const std::lock_guard<std::mutex> lock(mutex_);
  static_cast<void>(request);
  ++admissions_;
  ++counter_;
  RolloutAdmission admission;
  admission.admitted = admit_;
  admission.change_id = admit_ ? "change-" + std::to_string(counter_) : std::string();
  admission.window = "synthetic-window";
  admission.detail = admit_ ? "admitted" : detail_;
  if (!admit_) {
    admission.constraints.push_back(detail_);
  }
  return admission;
}

Status SyntheticRolloutFabric::stage_started(const std::string& change_id, const StageId& stage,
                                             const std::vector<TargetId>& targets, Timestamp at) {
  static_cast<void>(change_id);
  static_cast<void>(stage);
  static_cast<void>(targets);
  static_cast<void>(at);
  return ok_status();
}

Status SyntheticRolloutFabric::stage_completed(const std::string& change_id, const StageId& stage,
                                               Timestamp at) {
  static_cast<void>(change_id);
  static_cast<void>(stage);
  static_cast<void>(at);
  return ok_status();
}

Status SyntheticRolloutFabric::release(const std::string& change_id, Timestamp at) {
  static_cast<void>(change_id);
  static_cast<void>(at);
  return ok_status();
}

void SyntheticConfigurationFabric::set_failure(ErrorCode code) {
  const std::lock_guard<std::mutex> lock(mutex_);
  failure_ = code;
  has_failure_ = code != ErrorCode::ok;
}

std::size_t SyntheticConfigurationFabric::deliveries() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return deliveries_;
}

std::vector<std::string> SyntheticConfigurationFabric::delivered_versions() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return versions_;
}

Status SyntheticConfigurationFabric::deliver(const ConfigurationDelivery& delivery) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (has_failure_) {
    return make_error(failure_, "the synthetic configuration fabric refused delivery");
  }
  ++deliveries_;
  versions_.push_back(delivery.target.str() + "@" + delivery.version.text());
  return ok_status();
}

Result<std::string> SyntheticConfigurationFabric::current_revision(const TargetId& target) {
  const std::lock_guard<std::mutex> lock(mutex_);
  static_cast<void>(target);
  return std::string("synthetic-config-revision");
}

void SyntheticLedger::set_failure(ErrorCode code) {
  const std::lock_guard<std::mutex> lock(mutex_);
  failure_ = code;
  has_failure_ = code != ErrorCode::ok;
}

Status SyntheticLedger::append(const ProvenanceRecord& record) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (has_failure_) {
    return make_error(failure_, "the synthetic provenance ledger refused the record");
  }
  records_.push_back(record);
  return ok_status();
}

std::vector<ProvenanceRecord> SyntheticLedger::records() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return records_;
}

}  // namespace fum
