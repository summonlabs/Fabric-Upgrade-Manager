// Ports to the other Fabric systems and to vendor-specific installation
// mechanics.
//
// Fabric Upgrade Manager owns upgrade intent and execution lifecycle only. It
// does not implement compatibility knowledge, rollout sequencing, service
// draining or configuration delivery: those live in the Fabric Compatibility
// Registry, the Change Planner/Rollout Fabric, the Drain/Maintenance Fabric and
// the Configuration Fabric. When one of those systems is not wired, the engine
// reports not_integrated instead of guessing.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/time.hpp"
#include "fum/model/artifact.hpp"
#include "fum/model/campaign.hpp"
#include "fum/model/provenance.hpp"
#include "fum/model/target.hpp"

namespace fum {

// ---------------------------------------------------------------------------
// Fabric Compatibility Registry
// ---------------------------------------------------------------------------
struct [[nodiscard]] CompatibilityQuery {
  ComponentId component;
  Version from_version;
  Version to_version;
  BuildId to_build;
  std::string platform;
  std::vector<std::string> target_capabilities;
  StrategyKind strategy = StrategyKind::in_place;
  Incarnation incarnation;
  Timestamp at;
};

struct [[nodiscard]] CompatibilityVerdict {
  bool answered = false;         // false when the registry is not integrated
  bool compatible = false;
  std::string registry_revision;
  std::string registry_policy;
  std::vector<std::string> evaluated_constraints;
  std::vector<std::string> blockers;
  bool rollback_supported = false;
  std::string rollback_boundary;
  bool rollback_artifact_known = false;
  Duration validity = Duration::from_seconds(300);
  std::string detail;

  [[nodiscard]] json::Value to_json() const;
};

class CompatibilityRegistryPort {
 public:
  CompatibilityRegistryPort() = default;
  CompatibilityRegistryPort(const CompatibilityRegistryPort&) = delete;
  CompatibilityRegistryPort& operator=(const CompatibilityRegistryPort&) = delete;
  virtual ~CompatibilityRegistryPort();
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual Result<CompatibilityVerdict> query(const CompatibilityQuery& request) = 0;
};

// ---------------------------------------------------------------------------
// Change Planner / Rollout Fabric
// ---------------------------------------------------------------------------
struct [[nodiscard]] RolloutRequest {
  CampaignId campaign;
  AuthorityId authority;
  ComponentId component;
  std::string description;
  std::size_t target_count = 0;
  Incarnation incarnation;
  Timestamp at;
};

struct [[nodiscard]] RolloutAdmission {
  bool admitted = false;
  std::string change_id;
  std::string window;
  std::vector<std::string> constraints;
  std::string detail;

  [[nodiscard]] json::Value to_json() const;
};

class RolloutFabricPort {
 public:
  RolloutFabricPort() = default;
  RolloutFabricPort(const RolloutFabricPort&) = delete;
  RolloutFabricPort& operator=(const RolloutFabricPort&) = delete;
  virtual ~RolloutFabricPort();
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual Result<RolloutAdmission> admit(const RolloutRequest& request) = 0;
  [[nodiscard]] virtual Status stage_started(const std::string& change_id, const StageId& stage,
                                             const std::vector<TargetId>& targets,
                                             Timestamp at) = 0;
  [[nodiscard]] virtual Status stage_completed(const std::string& change_id, const StageId& stage,
                                               Timestamp at) = 0;
  [[nodiscard]] virtual Status release(const std::string& change_id, Timestamp at) = 0;
};

// ---------------------------------------------------------------------------
// Drain / Maintenance Fabric
// ---------------------------------------------------------------------------
struct [[nodiscard]] DrainRequest {
  TargetId target;
  ComponentId component;
  std::string reason;
  CorrelationId correlation;
  Timestamp at;
};

struct [[nodiscard]] DrainTicket {
  std::string ticket;
  TargetId target;
  Timestamp acquired_at;
  Timestamp expires_at;
  Duration validity = Duration::from_seconds(300);
  bool valid = false;
  std::string detail;

  [[nodiscard]] json::Value to_json() const;
};

class DrainFabricPort {
 public:
  DrainFabricPort() = default;
  DrainFabricPort(const DrainFabricPort&) = delete;
  DrainFabricPort& operator=(const DrainFabricPort&) = delete;
  virtual ~DrainFabricPort();
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual Result<DrainTicket> acquire(const DrainRequest& request) = 0;
  [[nodiscard]] virtual Status release(const DrainTicket& ticket) = 0;
  [[nodiscard]] virtual Result<bool> still_drained(const DrainTicket& ticket) = 0;
};

// ---------------------------------------------------------------------------
// Configuration Fabric
// ---------------------------------------------------------------------------
struct [[nodiscard]] ConfigurationDelivery {
  TargetId target;
  ComponentId component;
  Version version;
  BuildId build;
  std::string revision;
  Timestamp at;
};

class ConfigurationFabricPort {
 public:
  ConfigurationFabricPort() = default;
  ConfigurationFabricPort(const ConfigurationFabricPort&) = delete;
  ConfigurationFabricPort& operator=(const ConfigurationFabricPort&) = delete;
  virtual ~ConfigurationFabricPort();
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual Status deliver(const ConfigurationDelivery& delivery) = 0;
  [[nodiscard]] virtual Result<std::string> current_revision(const TargetId& target) = 0;
};

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------
struct [[nodiscard]] HealthReport {
  bool healthy = false;
  std::vector<std::string> checks;
  std::string detail;
  Timestamp observed_at;
  Duration validity = Duration::from_seconds(30);
  Incarnation incarnation;

  [[nodiscard]] json::Value to_json() const;
};

class HealthProbePort {
 public:
  HealthProbePort() = default;
  HealthProbePort(const HealthProbePort&) = delete;
  HealthProbePort& operator=(const HealthProbePort&) = delete;
  virtual ~HealthProbePort();
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual Result<HealthReport> probe(const TargetId& target, Timestamp now,
                                                   Incarnation incarnation) = 0;
};

// ---------------------------------------------------------------------------
// Artifact source
// ---------------------------------------------------------------------------
struct [[nodiscard]] ArtifactPayload {
  std::string path;       // local path to the resolved artifact bytes
  std::uint64_t bytes = 0;
  bool present = false;
  std::string detail;
};

class ArtifactSourcePort {
 public:
  ArtifactSourcePort() = default;
  ArtifactSourcePort(const ArtifactSourcePort&) = delete;
  ArtifactSourcePort& operator=(const ArtifactSourcePort&) = delete;
  virtual ~ArtifactSourcePort();
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual Result<ArtifactPayload> resolve(const ArtifactDescriptor& artifact) = 0;
};

// ---------------------------------------------------------------------------
// Provenance ledger sink (external)
// ---------------------------------------------------------------------------
class ProvenanceLedgerPort {
 public:
  ProvenanceLedgerPort() = default;
  ProvenanceLedgerPort(const ProvenanceLedgerPort&) = delete;
  ProvenanceLedgerPort& operator=(const ProvenanceLedgerPort&) = delete;
  virtual ~ProvenanceLedgerPort();
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual Status append(const ProvenanceRecord& record) = 0;
};

// ---------------------------------------------------------------------------
// Vendor-specific installation mechanics
// ---------------------------------------------------------------------------
// Idempotency key: repeating exactly this operation must be a no-op on the
// target. Every mutating adapter call carries one.
struct [[nodiscard]] OperationKey {
  CampaignId campaign;
  Generation generation;
  StageId stage;
  TargetId target;
  std::string operation;
  Sequence index;

  [[nodiscard]] std::string to_string() const;
  friend bool operator==(const OperationKey& a, const OperationKey& b) {
    return a.campaign == b.campaign && a.generation == b.generation && a.stage == b.stage &&
           a.target == b.target && a.operation == b.operation && a.index == b.index;
  }
};

struct [[nodiscard]] TargetOperation {
  FenceToken fence;
  OperationKey key;
  TargetDescriptor target;
  ArtifactDescriptor artifact;
  std::string payload_path;
  StrategyKind strategy = StrategyKind::in_place;
  Timestamp now;
  Epoch epoch;
  bool retry = false;

  [[nodiscard]] json::Value to_json() const;
};

struct [[nodiscard]] PrepareOutcome {
  bool prepared = false;
  bool already_prepared = false;
  std::string detail;
  std::vector<std::string> steps;

  [[nodiscard]] json::Value to_json() const;
};

struct [[nodiscard]] ActivateOutcome {
  bool activated = false;
  bool already_active = false;
  bool restart_performed = false;
  Version observed_version;
  BuildId observed_build;
  std::string detail;

  [[nodiscard]] json::Value to_json() const;
};

struct [[nodiscard]] VerifyOutcome {
  bool verified = false;
  bool healthy = false;
  Version observed_version;
  BuildId observed_build;
  std::vector<std::string> checks;
  std::string detail;
  Timestamp observed_at;
  Duration validity = Duration::from_seconds(60);
  Incarnation incarnation;

  [[nodiscard]] json::Value to_json() const;
};

struct [[nodiscard]] RollbackOutcome {
  bool rolled_back = false;
  bool already_rolled_back = false;
  Version observed_version;
  BuildId observed_build;
  std::string detail;

  [[nodiscard]] json::Value to_json() const;
};

class TargetAdapterPort {
 public:
  TargetAdapterPort() = default;
  TargetAdapterPort(const TargetAdapterPort&) = delete;
  TargetAdapterPort& operator=(const TargetAdapterPort&) = delete;
  virtual ~TargetAdapterPort();

  [[nodiscard]] virtual AdapterId id() const = 0;
  [[nodiscard]] virtual AdapterKind kind() const = 0;
  [[nodiscard]] virtual const AdapterClaims& claims() const = 0;

  [[nodiscard]] virtual Result<TargetObservation> observe(const TargetId& target, Timestamp now,
                                                          Incarnation incarnation) = 0;
  [[nodiscard]] virtual Result<PrepareOutcome> prepare(const TargetOperation& operation) = 0;
  [[nodiscard]] virtual Result<ActivateOutcome> activate(const TargetOperation& operation) = 0;
  [[nodiscard]] virtual Result<VerifyOutcome> verify(const TargetOperation& operation) = 0;
  [[nodiscard]] virtual Result<RollbackOutcome> rollback(const TargetOperation& operation) = 0;

  // Adapters that manage OS processes report whether their target is still
  // running; the engine never assumes it.
  [[nodiscard]] virtual bool manages_processes() const { return false; }
  [[nodiscard]] virtual Result<bool> process_alive(const TargetId& target) {
    static_cast<void>(target);
    return make_error(ErrorCode::unsupported, "adapter does not manage OS processes");
  }
};

// Adapter registry: target id -> adapter. Adapters are shared, never owned by
// the engine.
class [[nodiscard]] AdapterRegistry {
 public:
  void add(std::shared_ptr<TargetAdapterPort> adapter);
  [[nodiscard]] std::shared_ptr<TargetAdapterPort> find(const AdapterId& id) const;
  [[nodiscard]] std::vector<std::shared_ptr<TargetAdapterPort>> all() const;
  [[nodiscard]] std::size_t size() const;

 private:
  std::vector<std::shared_ptr<TargetAdapterPort>> adapters_;
};

}  // namespace fum
