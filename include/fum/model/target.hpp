// Upgrade targets and the truthful capability claims of their adapters.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/result.hpp"
#include "fum/core/time.hpp"
#include "fum/model/version.hpp"

namespace fum {

// Upgrade strategies. Each one is only usable when the target adapter
// truthfully claims it; the engine never assumes one on the adapter's behalf.
enum class StrategyKind : std::uint8_t {
  in_place,
  restart_based,
  redundant_pair_rolling,
  control_plane_generation_handoff,
};

[[nodiscard]] const char* strategy_kind_name(StrategyKind kind) noexcept;
[[nodiscard]] Result<StrategyKind> parse_strategy_kind(std::string_view text);

enum class AdapterKind : std::uint8_t { synthetic, local_process, external };

[[nodiscard]] const char* adapter_kind_name(AdapterKind kind) noexcept;
[[nodiscard]] Result<AdapterKind> parse_adapter_kind(std::string_view text);

// Adapter claims are declarations the runtime enforces against the adapter; an
// adapter that cannot do something must say so and the engine refuses to plan
// that strategy instead of pretending.
struct [[nodiscard]] AdapterClaims {
  std::vector<StrategyKind> strategies;
  bool supports_prepare = false;
  bool supports_rollback = false;
  bool supports_health_probe = false;
  bool supports_version_observation = false;
  bool supports_generation_handoff = false;
  bool requires_service_removal = false;   // drain/maintenance Fabric must be engaged
  std::uint32_t max_parallel_activations = 1;
  std::vector<std::string> declared_limits;

  [[nodiscard]] bool supports(StrategyKind kind) const;
  [[nodiscard]] Status validate() const;
  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<AdapterClaims> from_json(const json::Value& value);
};

struct [[nodiscard]] TargetDescriptor {
  TargetId id;
  ComponentId component;
  std::string display_name;
  Version installed_version;
  BuildId installed_build;
  std::string platform;
  std::vector<std::string> capabilities;
  AdapterId adapter;
  AdapterKind adapter_kind = AdapterKind::synthetic;
  AdapterClaims claims;
  AuthorityId authority;
  std::string redundancy_group;   // non-empty for members of a redundant pair/set
  bool is_control_plane = false;
  bool generation_capable = false;  // target can hand off a control-plane generation

  [[nodiscard]] Status validate() const;
  [[nodiscard]] bool has_capability(std::string_view name) const;
  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<TargetDescriptor> from_json(const json::Value& value);
};

// A point-in-time inventory observation. Inventory is evidence: it is bound to
// the process incarnation that observed it, so it can never become fresh again
// merely by being deserialized after a restart.
struct [[nodiscard]] InventorySnapshot {
  Revision revision;
  Incarnation incarnation;
  Timestamp observed_at;
  Duration validity;
  std::vector<TargetDescriptor> targets;

  [[nodiscard]] const TargetDescriptor* find(const TargetId& id) const;
  [[nodiscard]] std::vector<const TargetDescriptor*> members_of(std::string_view component) const;
};

struct [[nodiscard]] FreshnessVerdict {
  bool fresh = false;
  Duration age;
  std::string reason;

  [[nodiscard]] json::Value to_json() const;
};

[[nodiscard]] FreshnessVerdict evaluate_freshness(Timestamp observed_at, Duration validity,
                                                  Incarnation observed_in,
                                                  Timestamp now, Incarnation current);

}  // namespace fum
