#include "fum/adapters/local_process/local_process_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "fum/core/fs.hpp"
#include "fum/core/hash.hpp"
#include "control_plane_protocol.hpp"

namespace fum {
namespace {

constexpr const char* kPackageFile = "package.json";
constexpr const char* kPointerFile = "current.json";
constexpr const char* kPreviousFile = "previous.json";
constexpr const char* kRuntimeFile = "state/runtime.json";
constexpr const char* kInstalledExecutable = "control-plane.exe";

json::Value behavior_json(const LocalProcessBehavior& behavior) {
  json::Value value = json::Value::make_object();
  value.set("healthy", json::Value::make_bool(behavior.healthy));
  value.set("startup_delay_ms", json::Value::make_int(behavior.startup_delay_ms));
  value.set("crash_on_start", json::Value::make_bool(behavior.crash_on_start));
  return value;
}

Result<LocalProcessBehavior> parse_behavior(const json::Value& value) {
  LocalProcessBehavior behavior;
  if (const json::Value* healthy = value.find("healthy"); healthy != nullptr) {
    FUM_TRY(behavior.healthy, healthy->as_bool());
  }
  if (const json::Value* delay = value.find("startup_delay_ms"); delay != nullptr) {
    FUM_TRY(behavior.startup_delay_ms, delay->as_int());
  }
  if (const json::Value* crash = value.find("crash_on_start"); crash != nullptr) {
    FUM_TRY(behavior.crash_on_start, crash->as_bool());
  }
  return behavior;
}

}  // namespace

LocalProcessAdapter::LocalProcessAdapter(AdapterId id, LocalProcessOptions options)
    : id_(std::move(id)), options_(std::move(options)) {
  claims_.strategies = {StrategyKind::in_place, StrategyKind::restart_based};
  claims_.supports_prepare = true;
  claims_.supports_rollback = true;
  claims_.supports_health_probe = true;
  claims_.supports_version_observation = true;
  claims_.supports_generation_handoff = false;
  claims_.requires_service_removal = false;
  claims_.max_parallel_activations = 1;
  claims_.declared_limits.push_back(
      "one control-plane process per target: activations are serialised on the target");
  claims_.declared_limits.push_back(
      "redundant-pair rolling and generation handoff are not claimed by this adapter");
}

LocalProcessAdapter::~LocalProcessAdapter() {
  std::vector<TargetState*> states;
  {
    const std::lock_guard<std::mutex> lock(map_mutex_);
    for (auto& entry : targets_) {
      states.push_back(entry.second.get());
    }
  }
  for (auto* state : states) {
    const std::lock_guard<std::mutex> lock(state->mutex);
    if (!options_.keep_running_after_operations) {
      static_cast<void>(stop_process(*state));
    }
  }
}

void LocalProcessAdapter::add_target(LocalProcessTarget target) {
  auto state = std::make_unique<TargetState>();
  state->target = std::move(target);
  state->options = options_;
  state->behavior.healthy = state->target.healthy;
  {
    const std::lock_guard<std::mutex> lock(map_mutex_);
    targets_[state->target.id.str()] = std::move(state);
  }
}

void LocalProcessAdapter::set_behavior(const TargetId& target, LocalProcessBehavior behavior) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  state->behavior = behavior;
}

void LocalProcessAdapter::set_claims(AdapterClaims claims) { claims_ = std::move(claims); }

void LocalProcessAdapter::drop_next_activate_acknowledgement(const TargetId& target) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  state->drop_next_activate_ack = true;
}

void LocalProcessAdapter::drop_next_prepare_acknowledgement(const TargetId& target) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  state->drop_next_prepare_ack = true;
}

void LocalProcessAdapter::set_verify_stale(const TargetId& target, bool stale) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  state->verify_stale = stale;
}

void LocalProcessAdapter::set_verify_wrong_incarnation(const TargetId& target, bool value) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  state->verify_wrong_incarnation = value;
}

LocalProcessAdapter::TargetState* LocalProcessAdapter::find(const TargetId& target) {
  const std::lock_guard<std::mutex> lock(map_mutex_);
  const auto it = targets_.find(target.str());
  return it == targets_.end() ? nullptr : it->second.get();
}

void LocalProcessAdapter::log_message(LogLevel level, const std::string& message) const {
  if (logger_ != nullptr) {
    logger_->log(level, message);
  }
}

std::string LocalProcessAdapter::slot_path(const TargetId& target, const BuildId& build) const {
  const std::lock_guard<std::mutex> lock(map_mutex_);
  const auto it = targets_.find(target.str());
  if (it == targets_.end()) {
    return std::string();
  }
  return fs::join(fs::join(it->second->target.root_directory, "slots"), build.str());
}

std::size_t LocalProcessAdapter::restarts(const TargetId& target) const {
  const std::lock_guard<std::mutex> lock(map_mutex_);
  const auto it = targets_.find(target.str());
  if (it == targets_.end()) {
    return 0;
  }
  std::lock_guard<std::mutex> state_lock(it->second->mutex);
  return it->second->restarts;
}

Result<Version> LocalProcessAdapter::installed_version(const TargetState& state) const {
  const std::string pointer = fs::join(state.target.root_directory, kPointerFile);
  if (!fs::exists(pointer)) {
    return make_error(ErrorCode::not_found, "the target has no installed build pointer", pointer);
  }
  auto text = fs::read_file(pointer, 64 * 1024);
  if (!text.has_value()) {
    return text.error();
  }
  auto parsed = json::parse_object(text.value());
  if (!parsed.has_value()) {
    return parsed.error();
  }
  std::string_view version_text;
  FUM_TRY(version_text, parsed.value().require_string("version"));
  return Version::parse(version_text);
}

Status LocalProcessAdapter::ensure_link(TargetState& state, Timestamp now) {
  static_cast<void>(now);
  if (!state.supervisor.running() || state.port == 0) {
    return make_error(ErrorCode::not_found, "the target process is not running");
  }
  return ok_status();
}

Result<json::Value> LocalProcessAdapter::command(TargetState& state, const std::string& name) {
  FUM_TRYV(ensure_link(state, SystemClock::instance().now()));
  Sequence sequence = state.request_sequence;
  if (auto next = state.request_sequence.try_next(); next.has_value()) {
    state.request_sequence = next.value();
  }
  const json::Value request = lp::make_request(name, sequence, state.process_incarnation,
                                               state.options.token);
  // One connection per command: the control plane serves connections in turn,
  // so holding one open would starve every other observer.
  auto link = net::TcpConnection::connect_loopback(state.port);
  if (!link.has_value()) {
    return link.error();
  }
  net::TcpConnection connection = std::move(link).value();
  auto response = lp::send_command(connection, request);
  if (!response.has_value()) {
    return response.error();
  }
  auto ok = response.value().find("ok");
  if (ok == nullptr || !ok->is_bool()) {
    return make_error(ErrorCode::corrupt, "the control plane returned a malformed response");
  }
  if (!ok->as_bool().value_or(false)) {
    std::string detail;
    if (const json::Value* error = response.value().find("error"); error != nullptr) {
      detail = std::string(error->as_string().value_or(""));
    }
    return make_error(ErrorCode::precondition_failed,
                      "the control plane rejected the command: " + name, detail);
  }
  return response.value();
}

Status LocalProcessAdapter::start_process(TargetState& state, const std::string& build) {
  const std::string slot = fs::join(fs::join(state.target.root_directory, "slots"), build);
  const std::string executable = fs::join(slot, kInstalledExecutable);
  if (!fs::exists(executable)) {
    return make_error(ErrorCode::not_found, "the staged slot has no executable", executable);
  }
  const std::string state_directory = fs::join(state.target.root_directory, "state");
  FUM_TRYV(fs::ensure_directory(state_directory));

  proc::StartRequest request;
  request.executable = executable;
  request.arguments = {"--state", state_directory, "--port", "0", "--token", state.options.token,
                       "--target", state.target.id.str()};
  request.working_directory = slot;
  request.readiness_bound = state.options.readiness_bound;
  FUM_TRYV(state.supervisor.start(request));
  auto ready = state.supervisor.await_ready(request);
  if (!ready.has_value()) {
    return ready.error();
  }
  const std::string& line = ready.value().ready_line;
  const std::size_t first = line.find(' ');
  if (first == std::string::npos) {
    return make_error(ErrorCode::corrupt, "the target readiness line is malformed", line);
  }
  const std::size_t second = line.find(' ', first + 1);
  if (second == std::string::npos) {
    return make_error(ErrorCode::corrupt, "the target readiness line has no port", line);
  }
  const std::string port_text = line.substr(first + 1, second - first - 1);
  const std::string incarnation_text = line.substr(second + 1);
  std::uint64_t port = 0;
  std::uint64_t incarnation = 0;
  try {
    port = std::stoull(port_text);
    incarnation = std::stoull(incarnation_text);
  } catch (const std::exception&) {
    return make_error(ErrorCode::corrupt, "the target readiness line is not numeric", line);
  }
  if (port == 0 || port > 65535) {
    return make_error(ErrorCode::corrupt, "the target reported an out-of-range port", port_text);
  }
  state.port = static_cast<std::uint16_t>(port);
  state.process_incarnation = Incarnation(incarnation);
  state.current_build = build;
  ++state.restarts;
  log_message(LogLevel::debug,
              "target process started: " + state.target.id.str() + " build " + build + " pid " +
                  std::to_string(state.supervisor.pid()) + " incarnation " +
                  std::to_string(incarnation));
  return ok_status();
}

Status LocalProcessAdapter::stop_process(TargetState& state) {
  if (state.supervisor.running()) {
    // Ask the control plane to stop; the bounded wait below is the fallback.
    const auto response = command(state, "shutdown");
    static_cast<void>(response);
  }
  if (state.supervisor.pid() != 0) {
    const Status waited = state.supervisor.stop(state.options.shutdown_grace);
    if (!waited.has_value()) {
      const Status killed = state.supervisor.kill();
      if (!killed.has_value()) {
        return killed;
      }
    }
  }
  state.port = 0;
  return ok_status();
}

std::string LocalProcessAdapter::write_package(TargetState& state,
                                               const ArtifactDescriptor& artifact,
                                               std::string_view payload) const {
  const std::string slot = fs::join(fs::join(state.target.root_directory, "slots"),
                                    artifact.build().str());
  static_cast<void>(fs::ensure_directory(slot));
  const std::string executable = fs::join(slot, kInstalledExecutable);
  if (!fs::exists(executable)) {
    static_cast<void>(fs::copy_file(state.options.control_plane_executable, executable));
  }
  const std::string package = fs::join(slot, kPackageFile);
  std::string bytes(payload);
  if (bytes.empty()) {
    json::Value descriptor = json::Value::make_object();
    descriptor.set("version", json::Value::make_string(artifact.version().text()));
    descriptor.set("build", json::Value::make_string(artifact.build().str()));
    descriptor.set("component", json::Value::make_string(artifact.component().str()));
    descriptor.set("behavior", behavior_json(state.behavior));
    bytes = descriptor.dump();
  }
  static_cast<void>(fs::write_file_atomic(package, bytes));
  return slot;
}

Result<PrepareOutcome> LocalProcessAdapter::prepare(const TargetOperation& operation) {
  TargetState* state = find(operation.target.id);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      operation.target.id.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  std::string slot;
  if (!operation.payload_path.empty()) {
    auto payload = fs::read_file(operation.payload_path, 8ull * 1024 * 1024);
    if (!payload.has_value()) {
      return payload.error();
    }
    slot = write_package(*state, operation.artifact, payload.value());
  } else {
    slot = write_package(*state, operation.artifact, std::string_view());
  }
  const std::string package = fs::join(slot, kPackageFile);
  const IntegrityAssessment assessment = verify_artifact_file(operation.artifact, package);
  if (!assessment.verified) {
    return make_error(ErrorCode::integrity_failure,
                      "the staged package does not match the artifact descriptor",
                      assessment.detail);
  }
  state->staged_ready = true;
  state->staged_build = operation.artifact.build();
  state->staged_version = operation.artifact.version();
  if (state->drop_next_prepare_ack) {
    state->drop_next_prepare_ack = false;
    return make_error(ErrorCode::timeout,
                      "the prepare took effect but the acknowledgement was lost");
  }
  PrepareOutcome outcome;
  outcome.prepared = true;
  outcome.already_prepared = false;
  outcome.detail = "staged build " + operation.artifact.build().str() + " into " + slot;
  outcome.steps.push_back("copy-executable");
  outcome.steps.push_back("write-package-descriptor");
  outcome.steps.push_back("verify-digest");
  return outcome;
}

Result<ActivateOutcome> LocalProcessAdapter::activate(const TargetOperation& operation) {
  TargetState* state = find(operation.target.id);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      operation.target.id.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  const std::string build = operation.artifact.build().str();
  const std::string slot = fs::join(fs::join(state->target.root_directory, "slots"), build);
  if (!fs::exists(fs::join(slot, kInstalledExecutable))) {
    return make_error(ErrorCode::precondition_failed,
                      "the slot for this build has not been prepared", slot);
  }
  if (state->current_build == build && state->supervisor.running()) {
    ActivateOutcome outcome;
    outcome.activated = true;
    outcome.already_active = true;
    outcome.observed_version = operation.artifact.version();
    outcome.observed_build = operation.artifact.build();
    outcome.detail = "the target already runs this build";
    return outcome;
  }

  const std::string pointer = fs::join(state->target.root_directory, kPointerFile);
  if (fs::exists(pointer)) {
    const std::string previous = fs::join(state->target.root_directory, kPreviousFile);
    static_cast<void>(fs::copy_file(pointer, previous));
    auto previous_text = fs::read_file(pointer, 64 * 1024);
    if (previous_text.has_value()) {
      auto parsed = json::parse_object(previous_text.value());
      if (parsed.has_value()) {
        auto previous_build = parsed.value().require_string("build");
        if (previous_build.has_value()) {
          state->previous_build = std::string(previous_build.value());
        }
      }
    }
  }

  FUM_TRYV(stop_process(*state));

  json::Value record = json::Value::make_object();
  record.set("version", json::Value::make_string(operation.artifact.version().text()));
  record.set("build", json::Value::make_string(build));
  record.set("artifact_id", json::Value::make_string(operation.artifact.id().str()));
  record.set("digest", json::Value::make_string(operation.artifact.digest().hex()));
  record.set("activated_at", json::Value::make_string(operation.now.to_iso8601()));
  FUM_TRYV(fs::write_file_atomic(pointer, record.dump()));

  FUM_TRYV(start_process(*state, build));

  if (state->drop_next_activate_ack) {
    state->drop_next_activate_ack = false;
    return make_error(ErrorCode::timeout,
                      "the activation took effect but the acknowledgement was lost");
  }

  auto hello = command(*state, "hello");
  if (!hello.has_value()) {
    return hello.error();
  }
  ActivateOutcome outcome;
  std::string_view version_text;
  FUM_TRY(version_text, hello.value().require_string("version"));
  FUM_TRY(outcome.observed_version, Version::parse(version_text));
  std::string_view build_text;
  FUM_TRY(build_text, hello.value().require_string("build"));
  FUM_TRY(outcome.observed_build, BuildId::parse(build_text));
  outcome.activated = !outcome.observed_version.empty();
  outcome.restart_performed = true;
  outcome.detail = "restarted the control plane on build " + build + " (pid " +
                   std::to_string(state->supervisor.pid()) + ")";
  return outcome;
}

Result<VerifyOutcome> LocalProcessAdapter::verify(const TargetOperation& operation) {
  TargetState* state = find(operation.target.id);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      operation.target.id.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  VerifyOutcome outcome;
  outcome.observed_at = operation.now;
  outcome.validity = Duration::from_seconds(30);
  outcome.incarnation = operation.fence.incarnation;
  if (state->verify_stale) {
    outcome.observed_at = operation.now.plus(Duration::from_seconds(-3600));
  }
  if (state->verify_wrong_incarnation) {
    outcome.incarnation = Incarnation(operation.fence.incarnation.value() == 0
                                          ? 1
                                          : operation.fence.incarnation.value() - 1);
  }
  auto status = command(*state, "status");
  if (!status.has_value()) {
    outcome.verified = false;
    outcome.healthy = false;
    outcome.detail = status.error().to_string();
    return outcome;
  }
  std::string_view version_text;
  FUM_TRY(version_text, status.value().require_string("version"));
  FUM_TRY(outcome.observed_version, Version::parse(version_text));
  std::string_view build_text;
  FUM_TRY(build_text, status.value().require_string("build"));
  FUM_TRY(outcome.observed_build, BuildId::parse(build_text));
  FUM_TRY(outcome.healthy, status.value().require_bool("healthy"));
  std::string_view incarnation_text;
  FUM_TRY(incarnation_text, status.value().require_string("pid"));
  outcome.checks.push_back("control-plane-status");
  outcome.checks.push_back("running-build-matches-slot");
  outcome.verified = true;
  outcome.detail = "the control plane reports " + std::string(version_text) + " / " +
                   std::string(build_text) + " alive as pid " + std::string(incarnation_text) +
                   " on incarnation " + std::to_string(state->process_incarnation.value());
  if (!outcome.healthy) {
    outcome.detail.append("; reported unhealthy");
  }
  return outcome;
}

Result<RollbackOutcome> LocalProcessAdapter::rollback(const TargetOperation& operation) {
  TargetState* state = find(operation.target.id);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      operation.target.id.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  const std::string previous = fs::join(state->target.root_directory, kPreviousFile);
  if (!fs::exists(previous)) {
    return make_error(ErrorCode::precondition_failed,
                      "the target has no recorded previous build to restore", previous);
  }
  auto previous_text = fs::read_file(previous, 64 * 1024);
  if (!previous_text.has_value()) {
    return previous_text.error();
  }
  auto parsed = json::parse_object(previous_text.value());
  if (!parsed.has_value()) {
    return parsed.error();
  }
  std::string_view build_text;
  FUM_TRY(build_text, parsed.value().require_string("build"));
  std::string_view version_text;
  FUM_TRY(version_text, parsed.value().require_string("version"));
  const std::string build(build_text);
  const std::string slot = fs::join(fs::join(state->target.root_directory, "slots"), build);
  if (!fs::exists(fs::join(slot, kInstalledExecutable))) {
    return make_error(ErrorCode::precondition_failed,
                      "the previous build slot is no longer present", slot);
  }
  FUM_TRYV(stop_process(*state));
  const std::string pointer = fs::join(state->target.root_directory, kPointerFile);
  FUM_TRYV(fs::write_file_atomic(pointer, previous_text.value()));
  FUM_TRYV(start_process(*state, build));
  auto hello = command(*state, "hello");
  if (!hello.has_value()) {
    return hello.error();
  }
  RollbackOutcome outcome;
  FUM_TRY(outcome.observed_version, Version::parse(version_text));
  FUM_TRY(outcome.observed_build, BuildId::parse(build_text));
  outcome.rolled_back = true;
  outcome.detail = "restored build " + build + " (pid " +
                   std::to_string(state->supervisor.pid()) + ")";
  return outcome;
}

Result<TargetObservation> LocalProcessAdapter::observe(const TargetId& target, Timestamp now,
                                                       Incarnation incarnation) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      target.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  TargetObservation observation;
  observation.target = target;
  observation.observed_at = now;
  observation.incarnation = incarnation;
  observation.source = id_.str();
  if (state->supervisor.running() && state->port != 0) {
    auto status = command(*state, "status");
    if (status.has_value()) {
      std::string_view version_text;
      FUM_TRY(version_text, status.value().require_string("version"));
      FUM_TRY(observation.version, Version::parse(version_text));
      std::string_view build_text;
      FUM_TRY(build_text, status.value().require_string("build"));
      FUM_TRY(observation.build, BuildId::parse(build_text));
      FUM_TRY(observation.healthy, status.value().require_bool("healthy"));
      observation.health_detail = "the control plane answered on incarnation " +
                                  std::to_string(state->process_incarnation.value());
      return observation;
    }
  }
  auto version = installed_version(*state);
  if (version.has_value()) {
    observation.version = version.value();
  }
  observation.healthy = false;
  observation.health_known = false;   // no process, so no health to report
  observation.health_detail = "the target process is not running";
  return observation;
}

Result<bool> LocalProcessAdapter::process_alive(const TargetId& target) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      target.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return state->supervisor.running();
}

Status LocalProcessAdapter::kill_target_process(const TargetId& target) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      target.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return state->supervisor.kill();
}

Status LocalProcessAdapter::crash_target_process(const TargetId& target) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      target.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  if (state->supervisor.running()) {
    const auto response = command(*state, "crash");
    static_cast<void>(response);
  }
  auto waited = state->supervisor.wait_exit(Duration::from_seconds(10));
  if (!waited.has_value()) {
    return state->supervisor.kill();
  }
  return ok_status();
}

Status LocalProcessAdapter::restart_target_process(const TargetId& target) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      target.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  const std::string build = state->current_build;
  if (build.empty()) {
    return make_error(ErrorCode::precondition_failed, "the target has never been started");
  }
  FUM_TRYV(stop_process(*state));
  return start_process(*state, build);
}

Result<LocalProcessAdapter::RuntimeFacts> LocalProcessAdapter::runtime_facts(
    const TargetId& target) {
  TargetState* state = find(target);
  if (state == nullptr) {
    return make_error(ErrorCode::not_found, "the local-process adapter does not know this target",
                      target.str());
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  RuntimeFacts facts;
  facts.pid = state->supervisor.pid();
  facts.incarnation = state->process_incarnation;
  facts.port = state->port;
  facts.alive = state->supervisor.running();
  facts.current_build = state->current_build;
  const std::string pointer = fs::join(state->target.root_directory, kPointerFile);
  if (fs::exists(pointer)) {
    auto text = fs::read_file(pointer, 64 * 1024);
    if (text.has_value()) {
      auto parsed = json::parse_object(text.value());
      if (parsed.has_value()) {
        auto version_text = parsed.value().require_string("version");
        auto build_text = parsed.value().require_string("build");
        if (version_text.has_value()) {
          auto version = Version::parse(version_text.value());
          if (version.has_value()) {
            facts.version = version.value();
          }
        }
        if (build_text.has_value()) {
          auto build = BuildId::parse(build_text.value());
          if (build.has_value()) {
            facts.build = build.value();
          }
        }
      }
    }
  }
  const std::string runtime = fs::join(state->target.root_directory, kRuntimeFile);
  if (fs::exists(runtime)) {
    auto text = fs::read_file(runtime, 64 * 1024);
    if (text.has_value()) {
      facts.detail = text.value();
    }
  }
  return facts;
}

}  // namespace fum