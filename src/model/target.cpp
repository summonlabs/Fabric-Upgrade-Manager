#include "fum/model/target.hpp"

#include <algorithm>

namespace fum {
namespace {

Result<std::string> optional_string(const json::Value& value, std::string_view key) {
  const json::Value* found = value.find(key);
  if (found == nullptr) {
    return std::string();
  }
  std::string_view text;
  FUM_TRY(text, found->as_string());
  return std::string(text);
}

}  // namespace

const char* strategy_kind_name(StrategyKind kind) noexcept {
  switch (kind) {
    case StrategyKind::in_place: return "in-place";
    case StrategyKind::restart_based: return "restart-based";
    case StrategyKind::redundant_pair_rolling: return "redundant-pair-rolling";
    case StrategyKind::control_plane_generation_handoff: return "control-plane-generation-handoff";
  }
  return "unknown";
}

Result<StrategyKind> parse_strategy_kind(std::string_view text) {
  if (text == "in-place") return StrategyKind::in_place;
  if (text == "restart-based") return StrategyKind::restart_based;
  if (text == "redundant-pair-rolling") return StrategyKind::redundant_pair_rolling;
  if (text == "control-plane-generation-handoff") {
    return StrategyKind::control_plane_generation_handoff;
  }
  return make_error(ErrorCode::invalid_argument, "unknown upgrade strategy", std::string(text));
}

const char* adapter_kind_name(AdapterKind kind) noexcept {
  switch (kind) {
    case AdapterKind::synthetic: return "synthetic";
    case AdapterKind::local_process: return "local-process";
    case AdapterKind::external: return "external";
  }
  return "unknown";
}

Result<AdapterKind> parse_adapter_kind(std::string_view text) {
  if (text == "synthetic") return AdapterKind::synthetic;
  if (text == "local-process") return AdapterKind::local_process;
  if (text == "external") return AdapterKind::external;
  return make_error(ErrorCode::invalid_argument, "unknown adapter kind", std::string(text));
}

bool AdapterClaims::supports(StrategyKind kind) const {
  return std::find(strategies.begin(), strategies.end(), kind) != strategies.end();
}

Status AdapterClaims::validate() const {
  if (strategies.empty()) {
    return make_error(ErrorCode::invalid_argument, "an adapter must claim at least one strategy");
  }
  std::vector<StrategyKind> sorted = strategies;
  std::sort(sorted.begin(), sorted.end(),
            [](StrategyKind a, StrategyKind b) {
              return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
            });
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    return make_error(ErrorCode::invalid_argument, "adapter claims a strategy twice");
  }
  if (supports(StrategyKind::control_plane_generation_handoff) && !supports_generation_handoff) {
    return make_error(ErrorCode::invalid_argument,
                      "generation handoff strategy claimed without the generation handoff "
                      "capability");
  }
  if (supports(StrategyKind::redundant_pair_rolling) && !supports_health_probe) {
    return make_error(ErrorCode::invalid_argument,
                      "rolling replacement requires a health probe capability");
  }
  if (max_parallel_activations == 0) {
    return make_error(ErrorCode::invalid_argument, "max_parallel_activations must be at least 1");
  }
  if (max_parallel_activations > 256) {
    return make_error(ErrorCode::invalid_argument,
                      "max_parallel_activations exceeds the permitted bound");
  }
  return ok_status();
}

json::Value AdapterClaims::to_json() const {
  json::Value value = json::Value::make_object();
  json::Value kinds = json::Value::make_array();
  for (const auto kind : strategies) {
    kinds.push(json::Value::make_string(strategy_kind_name(kind)));
  }
  value.set("strategies", std::move(kinds));
  value.set("supports_prepare", json::Value::make_bool(supports_prepare));
  value.set("supports_rollback", json::Value::make_bool(supports_rollback));
  value.set("supports_health_probe", json::Value::make_bool(supports_health_probe));
  value.set("supports_version_observation", json::Value::make_bool(supports_version_observation));
  value.set("supports_generation_handoff", json::Value::make_bool(supports_generation_handoff));
  value.set("requires_service_removal", json::Value::make_bool(requires_service_removal));
  value.set("max_parallel_activations", json::Value::make_uint(max_parallel_activations));
  json::Value limits = json::Value::make_array();
  for (const auto& limit : declared_limits) {
    limits.push(json::Value::make_string(limit));
  }
  value.set("declared_limits", std::move(limits));
  return value;
}

Result<AdapterClaims> AdapterClaims::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "adapter claims must be an object");
  }
  AdapterClaims out;
  const json::Value* kinds = value.find("strategies");
  if (kinds == nullptr || !kinds->is_array()) {
    return make_error(ErrorCode::invalid_argument, "adapter claims require a strategies array");
  }
  for (const auto& item : kinds->items()) {
    std::string_view text;
    FUM_TRY(text, item.as_string());
    StrategyKind kind = StrategyKind::in_place;
    FUM_TRY(kind, parse_strategy_kind(text));
    out.strategies.push_back(kind);
  }
  const json::Value* prepare = value.find("supports_prepare");
  if (prepare != nullptr) {
    FUM_TRY(out.supports_prepare, prepare->as_bool());
  }
  const json::Value* rollback = value.find("supports_rollback");
  if (rollback != nullptr) {
    FUM_TRY(out.supports_rollback, rollback->as_bool());
  }
  const json::Value* health = value.find("supports_health_probe");
  if (health != nullptr) {
    FUM_TRY(out.supports_health_probe, health->as_bool());
  }
  const json::Value* version_observation = value.find("supports_version_observation");
  if (version_observation != nullptr) {
    FUM_TRY(out.supports_version_observation, version_observation->as_bool());
  }
  const json::Value* handoff = value.find("supports_generation_handoff");
  if (handoff != nullptr) {
    FUM_TRY(out.supports_generation_handoff, handoff->as_bool());
  }
  const json::Value* drain = value.find("requires_service_removal");
  if (drain != nullptr) {
    FUM_TRY(out.requires_service_removal, drain->as_bool());
  }
  const json::Value* parallel = value.find("max_parallel_activations");
  if (parallel != nullptr) {
    const std::uint64_t count = [&]() -> std::uint64_t {
      auto parsed = parallel->as_uint();
      return parsed.has_value() ? parsed.value() : 0;
    }();
    out.max_parallel_activations = static_cast<std::uint32_t>(count);
  }
  FUM_TRYV(out.validate());
  return out;
}

bool TargetDescriptor::has_capability(std::string_view name) const {
  return std::find(capabilities.begin(), capabilities.end(), name) != capabilities.end();
}

Status TargetDescriptor::validate() const {
  if (id.empty()) {
    return make_error(ErrorCode::invalid_argument, "target id is required");
  }
  if (component.empty()) {
    return make_error(ErrorCode::invalid_argument, "target component id is required");
  }
  if (platform.empty()) {
    return make_error(ErrorCode::invalid_argument, "target platform is required");
  }
  if (adapter.empty()) {
    return make_error(ErrorCode::invalid_argument, "target adapter id is required");
  }
  if (authority.empty()) {
    return make_error(ErrorCode::invalid_argument, "target authority is required");
  }
  FUM_TRYV(claims.validate());
  return ok_status();
}

json::Value TargetDescriptor::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("target_id", json::Value::make_string(id.str()));
  value.set("component_id", json::Value::make_string(component.str()));
  value.set("display_name", json::Value::make_string(display_name));
  value.set("installed_version", json::Value::make_string(installed_version.text()));
  value.set("installed_build", json::Value::make_string(installed_build.str()));
  value.set("platform", json::Value::make_string(platform));
  json::Value caps = json::Value::make_array();
  for (const auto& capability : capabilities) {
    caps.push(json::Value::make_string(capability));
  }
  value.set("capabilities", std::move(caps));
  value.set("adapter_id", json::Value::make_string(adapter.str()));
  value.set("adapter_kind", json::Value::make_string(adapter_kind_name(adapter_kind)));
  value.set("claims", claims.to_json());
  value.set("authority", json::Value::make_string(authority.str()));
  value.set("redundancy_group", json::Value::make_string(redundancy_group));
  value.set("is_control_plane", json::Value::make_bool(is_control_plane));
  value.set("generation_capable", json::Value::make_bool(generation_capable));
  return value;
}

Result<TargetDescriptor> TargetDescriptor::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "target descriptor must be a json object");
  }
  TargetDescriptor out;
  std::string text;
  FUM_TRY(text, optional_string(value, "target_id"));
  FUM_TRY(out.id, TargetId::parse(text));
  FUM_TRY(text, optional_string(value, "component_id"));
  FUM_TRY(out.component, ComponentId::parse(text));
  FUM_TRY(out.display_name, optional_string(value, "display_name"));
  std::string_view version_text;
  FUM_TRY(version_text, value.require_string("installed_version"));
  FUM_TRY(out.installed_version, Version::parse(version_text));
  FUM_TRY(text, optional_string(value, "installed_build"));
  if (!text.empty()) {
    FUM_TRY(out.installed_build, BuildId::parse(text));
  }
  FUM_TRY(out.platform, optional_string(value, "platform"));
  const json::Value* caps = value.find("capabilities");
  if (caps != nullptr && caps->is_array()) {
    for (const auto& item : caps->items()) {
      std::string_view capability;
      FUM_TRY(capability, item.as_string());
      out.capabilities.emplace_back(capability);
    }
  }
  FUM_TRY(text, optional_string(value, "adapter_id"));
  FUM_TRY(out.adapter, AdapterId::parse(text));
  const json::Value* kind = value.find("adapter_kind");
  if (kind != nullptr) {
    std::string_view kind_text;
    FUM_TRY(kind_text, kind->as_string());
    FUM_TRY(out.adapter_kind, parse_adapter_kind(kind_text));
  }
  const json::Value* claims = value.find("claims");
  if (claims == nullptr) {
    return make_error(ErrorCode::invalid_argument, "target descriptor requires adapter claims");
  }
  FUM_TRY(out.claims, AdapterClaims::from_json(*claims));
  FUM_TRY(text, optional_string(value, "authority"));
  FUM_TRY(out.authority, AuthorityId::parse(text));
  FUM_TRY(out.redundancy_group, optional_string(value, "redundancy_group"));
  const json::Value* control_plane = value.find("is_control_plane");
  if (control_plane != nullptr) {
    FUM_TRY(out.is_control_plane, control_plane->as_bool());
  }
  const json::Value* generation_capable = value.find("generation_capable");
  if (generation_capable != nullptr) {
    FUM_TRY(out.generation_capable, generation_capable->as_bool());
  }
  FUM_TRYV(out.validate());
  return out;
}

const TargetDescriptor* InventorySnapshot::find(const TargetId& id) const {
  for (const auto& target : targets) {
    if (target.id == id) {
      return &target;
    }
  }
  return nullptr;
}

std::vector<const TargetDescriptor*> InventorySnapshot::members_of(
    std::string_view component) const {
  std::vector<const TargetDescriptor*> out;
  for (const auto& target : targets) {
    if (target.component.str() == component) {
      out.push_back(&target);
    }
  }
  std::sort(out.begin(), out.end(), [](const TargetDescriptor* a, const TargetDescriptor* b) {
    return a->id < b->id;
  });
  return out;
}

json::Value FreshnessVerdict::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("fresh", json::Value::make_bool(fresh));
  value.set("age_nanos", json::Value::make_int(age.nanos()));
  value.set("reason", json::Value::make_string(reason));
  return value;
}

FreshnessVerdict evaluate_freshness(Timestamp observed_at, Duration validity,
                                    Incarnation observed_in, Timestamp now,
                                    Incarnation current) {
  FreshnessVerdict verdict;
  verdict.age = now.since(observed_at);
  if (observed_in != current) {
    verdict.fresh = false;
    verdict.reason = "observed in incarnation " + std::to_string(observed_in.value()) +
                     " but the current incarnation is " + std::to_string(current.value());
    return verdict;
  }
  if (validity.is_zero() || validity.is_negative()) {
    verdict.fresh = false;
    verdict.reason = "evidence declares no positive validity window";
    return verdict;
  }
  if (verdict.age.is_negative()) {
    verdict.fresh = false;
    verdict.reason = "evidence is stamped in the future relative to the runtime clock";
    return verdict;
  }
  if (verdict.age > validity) {
    verdict.fresh = false;
    verdict.reason = "evidence is older than its validity window (" +
                     std::to_string(verdict.age.nanos()) + "ns > " +
                     std::to_string(validity.nanos()) + "ns)";
    return verdict;
  }
  verdict.fresh = true;
  verdict.reason = "fresh";
  return verdict;
}

}  // namespace fum
