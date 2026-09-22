#include "fum/ports/ports.hpp"

#include <algorithm>

namespace fum {

CompatibilityRegistryPort::~CompatibilityRegistryPort() = default;
RolloutFabricPort::~RolloutFabricPort() = default;
DrainFabricPort::~DrainFabricPort() = default;
ConfigurationFabricPort::~ConfigurationFabricPort() = default;
HealthProbePort::~HealthProbePort() = default;
ArtifactSourcePort::~ArtifactSourcePort() = default;
ProvenanceLedgerPort::~ProvenanceLedgerPort() = default;
TargetAdapterPort::~TargetAdapterPort() = default;

json::Value CompatibilityVerdict::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("answered", json::Value::make_bool(answered));
  value.set("compatible", json::Value::make_bool(compatible));
  value.set("registry_revision", json::Value::make_string(registry_revision));
  value.set("registry_policy", json::Value::make_string(registry_policy));
  json::Value constraints = json::Value::make_array();
  for (const auto& item : evaluated_constraints) {
    constraints.push(json::Value::make_string(item));
  }
  value.set("evaluated_constraints", std::move(constraints));
  json::Value blockers_json = json::Value::make_array();
  for (const auto& item : blockers) {
    blockers_json.push(json::Value::make_string(item));
  }
  value.set("blockers", std::move(blockers_json));
  value.set("rollback_supported", json::Value::make_bool(rollback_supported));
  value.set("rollback_boundary", json::Value::make_string(rollback_boundary));
  value.set("rollback_artifact_known", json::Value::make_bool(rollback_artifact_known));
  value.set("validity_nanos", json::Value::make_int(validity.nanos()));
  value.set("detail", json::Value::make_string(detail));
  return value;
}

json::Value RolloutAdmission::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("admitted", json::Value::make_bool(admitted));
  value.set("change_id", json::Value::make_string(change_id));
  value.set("window", json::Value::make_string(window));
  json::Value constraints_json = json::Value::make_array();
  for (const auto& item : constraints) {
    constraints_json.push(json::Value::make_string(item));
  }
  value.set("constraints", std::move(constraints_json));
  value.set("detail", json::Value::make_string(detail));
  return value;
}

json::Value DrainTicket::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("ticket", json::Value::make_string(ticket));
  value.set("target_id", json::Value::make_string(target.str()));
  value.set("acquired_at", json::Value::make_string(acquired_at.to_iso8601()));
  value.set("expires_at", json::Value::make_string(expires_at.to_iso8601()));
  value.set("validity_nanos", json::Value::make_int(validity.nanos()));
  value.set("valid", json::Value::make_bool(valid));
  value.set("detail", json::Value::make_string(detail));
  return value;
}

json::Value HealthReport::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("healthy", json::Value::make_bool(healthy));
  json::Value checks_json = json::Value::make_array();
  for (const auto& item : checks) {
    checks_json.push(json::Value::make_string(item));
  }
  value.set("checks", std::move(checks_json));
  value.set("detail", json::Value::make_string(detail));
  value.set("observed_at", json::Value::make_string(observed_at.to_iso8601()));
  value.set("validity_nanos", json::Value::make_int(validity.nanos()));
  value.set("incarnation", json::Value::make_uint(incarnation.value()));
  return value;
}

std::string OperationKey::to_string() const {
  std::string out;
  out.reserve(96);
  out.append(campaign.str());
  out.push_back('/');
  out.append(std::to_string(generation.value()));
  out.push_back('/');
  out.append(stage.str());
  out.push_back('/');
  out.append(target.str());
  out.push_back('/');
  out.append(operation);
  out.push_back('#');
  out.append(std::to_string(index.value()));
  return out;
}

json::Value TargetOperation::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("fence", json::Value::make_string(to_string(fence)));
  value.set("operation_key", json::Value::make_string(key.to_string()));
  value.set("target_id", json::Value::make_string(target.id.str()));
  value.set("artifact_id", json::Value::make_string(artifact.id().str()));
  value.set("artifact_digest", json::Value::make_string(artifact.digest().hex()));
  value.set("payload_path", json::Value::make_string(payload_path));
  value.set("strategy", json::Value::make_string(strategy_kind_name(strategy)));
  value.set("now", json::Value::make_string(now.to_iso8601()));
  value.set("epoch", json::Value::make_uint(epoch.value()));
  value.set("retry", json::Value::make_bool(retry));
  return value;
}

json::Value PrepareOutcome::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("prepared", json::Value::make_bool(prepared));
  value.set("already_prepared", json::Value::make_bool(already_prepared));
  value.set("detail", json::Value::make_string(detail));
  json::Value steps_json = json::Value::make_array();
  for (const auto& item : steps) {
    steps_json.push(json::Value::make_string(item));
  }
  value.set("steps", std::move(steps_json));
  return value;
}

json::Value ActivateOutcome::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("activated", json::Value::make_bool(activated));
  value.set("already_active", json::Value::make_bool(already_active));
  value.set("restart_performed", json::Value::make_bool(restart_performed));
  value.set("observed_version", json::Value::make_string(observed_version.text()));
  value.set("observed_build", json::Value::make_string(observed_build.str()));
  value.set("detail", json::Value::make_string(detail));
  return value;
}

json::Value VerifyOutcome::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("verified", json::Value::make_bool(verified));
  value.set("healthy", json::Value::make_bool(healthy));
  value.set("observed_version", json::Value::make_string(observed_version.text()));
  value.set("observed_build", json::Value::make_string(observed_build.str()));
  json::Value checks_json = json::Value::make_array();
  for (const auto& item : checks) {
    checks_json.push(json::Value::make_string(item));
  }
  value.set("checks", std::move(checks_json));
  value.set("detail", json::Value::make_string(detail));
  value.set("observed_at", json::Value::make_string(observed_at.to_iso8601()));
  value.set("validity_nanos", json::Value::make_int(validity.nanos()));
  value.set("incarnation", json::Value::make_uint(incarnation.value()));
  return value;
}

json::Value RollbackOutcome::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("rolled_back", json::Value::make_bool(rolled_back));
  value.set("already_rolled_back", json::Value::make_bool(already_rolled_back));
  value.set("observed_version", json::Value::make_string(observed_version.text()));
  value.set("observed_build", json::Value::make_string(observed_build.str()));
  value.set("detail", json::Value::make_string(detail));
  return value;
}

void AdapterRegistry::add(std::shared_ptr<TargetAdapterPort> adapter) {
  if (!adapter) {
    return;
  }
  for (auto& existing : adapters_) {
    if (existing->id() == adapter->id()) {
      existing = std::move(adapter);
      return;
    }
  }
  adapters_.push_back(std::move(adapter));
  std::sort(adapters_.begin(), adapters_.end(),
            [](const std::shared_ptr<TargetAdapterPort>& a,
               const std::shared_ptr<TargetAdapterPort>& b) { return a->id() < b->id(); });
}

std::shared_ptr<TargetAdapterPort> AdapterRegistry::find(const AdapterId& id) const {
  for (const auto& adapter : adapters_) {
    if (adapter->id() == id) {
      return adapter;
    }
  }
  return nullptr;
}

std::vector<std::shared_ptr<TargetAdapterPort>> AdapterRegistry::all() const {
  return adapters_;
}

std::size_t AdapterRegistry::size() const { return adapters_.size(); }

}  // namespace fum
