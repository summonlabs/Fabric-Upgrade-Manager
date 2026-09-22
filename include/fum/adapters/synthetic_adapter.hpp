// Synthetic Fabric: deterministic, cross-platform stand-ins for the external
// systems and for vendor targets.
//
// These adapters are honest about being synthetic: they simulate installation
// mechanics, target health and fabric answers deterministically, and they are
// the proof surface for every governance invariant that does not require real
// hardware.
#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fum/engine/engine.hpp"
#include "fum/ports/ports.hpp"

namespace fum {

// Deterministic fault injection points.
enum class SyntheticFault : std::uint8_t {
  none = 0,
  prepare_failure,
  prepare_timeout,          // acknowledgement lost before prepare took effect
  activate_failure,
  activate_timeout_effect,  // activation takes effect but the acknowledgement is lost
  activate_timeout_noop,    // activation never takes effect and the acknowledgement is lost
  activate_wrong_version,   // reports success but the observed version stays wrong
  verify_failure,
  verify_unhealthy,
  verify_stale,             // verification evidence is older than its validity window
  verify_foreign_incarnation,  // evidence claims a previous process incarnation
  rollback_failure,
  observe_failure,
  crash_target,             // the target process dies after applying the activation
};

[[nodiscard]] const char* synthetic_fault_name(SyntheticFault fault) noexcept;

// Faults are scoped: a queued activate fault is not consumed by an observation
// of the same target, so tests stay deterministic about which call is affected.
enum class SyntheticFaultScope : std::uint8_t { prepare, activate, verify, rollback, observe };

[[nodiscard]] SyntheticFaultScope synthetic_fault_scope(SyntheticFault fault) noexcept;

// A one-shot gate: tests hold an adapter operation open while the runtime is
// mutated (pause, abort, shutdown), which is how cancellation and stale-commit
// paths are exercised deterministically.
class [[nodiscard]] SyntheticGate {
 public:
  void open();
  void wait();
  // Blocks until an operation has taken this gate. Used instead of polling so
  // tests never rely on timing.
  void wait_until_reached();
  void mark_reached();
  [[nodiscard]] bool opened() const;
  [[nodiscard]] bool reached() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable reached_cv_;
  bool open_ = false;
  bool reached_ = false;
};

struct [[nodiscard]] SyntheticTarget {
  TargetId id;
  ComponentId component;
  std::string display_name;
  Version version;
  BuildId build;
  std::string platform = "synthetic/x86_64";
  std::vector<std::string> capabilities;
  bool healthy = true;
  std::string health_detail = "synthetic health ok";
  Duration health_validity = Duration::from_seconds(30);
  Duration health_age_offset;
  bool process_alive = true;
  bool generation_capable = false;
  std::string redundancy_group;
  AuthorityId authority;
};

class [[nodiscard]] SyntheticAdapter : public TargetAdapterPort {
 public:
  explicit SyntheticAdapter(AdapterId id, AdapterKind kind = AdapterKind::synthetic);

  void set_claims(AdapterClaims claims);
  void add_target(SyntheticTarget target);
  void remove_target(const TargetId& target);

  // Fault programme: consumed by the next matching operation on that target.
  void enqueue_fault(const TargetId& target, SyntheticFault fault);
  void set_persistent_fault(const TargetId& target, SyntheticFault fault);
  void clear_faults(const TargetId& target);
  void set_health(const TargetId& target, bool healthy, std::string detail);
  void set_health_validity(const TargetId& target, Duration validity);
  void set_health_age_offset(const TargetId& target, Duration offset);
  void set_process_alive(const TargetId& target, bool alive);
  void set_version(const TargetId& target, Version version, BuildId build);
  // Holds the next activate/verify for this target on the gate.
  void gate_next_activate(const TargetId& target, std::shared_ptr<SyntheticGate> gate);
  void gate_next_verify(const TargetId& target, std::shared_ptr<SyntheticGate> gate);

  [[nodiscard]] AdapterId id() const override { return id_; }
  [[nodiscard]] AdapterKind kind() const override { return kind_; }
  [[nodiscard]] const AdapterClaims& claims() const override { return claims_; }

  [[nodiscard]] Result<TargetObservation> observe(const TargetId& target, Timestamp now,
                                                  Incarnation incarnation) override;
  [[nodiscard]] Result<PrepareOutcome> prepare(const TargetOperation& operation) override;
  [[nodiscard]] Result<ActivateOutcome> activate(const TargetOperation& operation) override;
  [[nodiscard]] Result<VerifyOutcome> verify(const TargetOperation& operation) override;
  [[nodiscard]] Result<RollbackOutcome> rollback(const TargetOperation& operation) override;
  [[nodiscard]] bool manages_processes() const override { return true; }
  [[nodiscard]] Result<bool> process_alive(const TargetId& target) override;

  struct Counters {
    std::uint64_t prepare_calls = 0;
    std::uint64_t activate_calls = 0;
    std::uint64_t verify_calls = 0;
    std::uint64_t rollback_calls = 0;
    std::uint64_t observe_calls = 0;
    std::uint64_t faults_injected = 0;
    std::uint64_t duplicate_operations = 0;
  };

  [[nodiscard]] Counters counters() const;
  [[nodiscard]] std::vector<std::string> operation_log() const;
  [[nodiscard]] Version version_of(const TargetId& target) const;

 private:
  struct State {
    SyntheticTarget target;
    Version staged_version;
    BuildId staged_build;
    Version previous_version;
    BuildId previous_build;
    std::deque<SyntheticFault> faults;
    SyntheticFault persistent = SyntheticFault::none;
    std::vector<std::string> completed_operations;
  };

  [[nodiscard]] Result<State*> find(const TargetId& target);
  [[nodiscard]] SyntheticFault next_fault(State& state, SyntheticFaultScope scope);
  [[nodiscard]] std::shared_ptr<SyntheticGate> take_gate(const TargetId& target, bool activate);
  [[nodiscard]] bool already_done(State& state, const std::string& key);

  mutable std::mutex mutex_;
  AdapterId id_;
  AdapterKind kind_;
  AdapterClaims claims_;
  std::map<std::string, State> targets_;
  std::deque<std::shared_ptr<SyntheticGate>> activate_gates_;
  std::deque<std::shared_ptr<SyntheticGate>> verify_gates_;
  Counters counters_;
  std::vector<std::string> operation_log_;
  std::size_t max_log_entries_ = 4096;
};

// ---------------------------------------------------------------------------
// Fabric stand-ins
// ---------------------------------------------------------------------------
struct [[nodiscard]] CompatibilityMatrixEntry {
  ComponentId component;
  Version from;
  Version to;
  bool compatible = false;
  std::string reason;
  bool rollback_supported = false;
  std::string rollback_boundary;
};

class [[nodiscard]] SyntheticCompatibilityRegistry : public CompatibilityRegistryPort {
 public:
  explicit SyntheticCompatibilityRegistry(std::string revision = "registry-rev-1");

  void add_entry(CompatibilityMatrixEntry entry);
  void set_revision(std::string revision);
  void set_answered(bool answered) { answered_ = answered; }
  void set_query_failure(ErrorCode code) { failure_ = code; }
  void set_default_compatible(bool value) { default_compatible_ = value; }

  [[nodiscard]] std::string id() const override { return "synthetic-compatibility-registry"; }
  [[nodiscard]] Result<CompatibilityVerdict> query(const CompatibilityQuery& request) override;
  [[nodiscard]] std::size_t queries() const;

 private:
  mutable std::mutex mutex_;
  std::string revision_;
  std::vector<CompatibilityMatrixEntry> entries_;
  bool answered_ = true;
  bool has_failure_ = false;
  ErrorCode failure_ = ErrorCode::ok;
  bool default_compatible_ = false;
  std::size_t queries_ = 0;
};

class [[nodiscard]] SyntheticInventory : public InventorySourcePort {
 public:
  void set_targets(std::vector<TargetDescriptor> targets);
  void add_target(TargetDescriptor target);
  void set_resolve_failure(ErrorCode code);
  void set_fail_next_resolve(bool value) { fail_next_ = value; }

  [[nodiscard]] std::string id() const override { return "synthetic-inventory"; }
  [[nodiscard]] Result<std::vector<TargetDescriptor>> resolve(Timestamp now,
                                                             Incarnation incarnation) override;
  [[nodiscard]] std::size_t resolves() const;

 private:
  mutable std::mutex mutex_;
  std::vector<TargetDescriptor> targets_;
  bool has_failure_ = false;
  ErrorCode failure_ = ErrorCode::ok;
  bool fail_next_ = false;
  std::size_t resolves_ = 0;
};

// Writes artifact bytes to a temporary directory and can corrupt them on
// purpose so that integrity gating can be proven.
class [[nodiscard]] SyntheticArtifactSource : public ArtifactSourcePort {
 public:
  SyntheticArtifactSource();
  ~SyntheticArtifactSource() override;

  // Materialises the payload for an artifact. When corrupt is true the bytes on
  // disk do not match the declared digest.
  [[nodiscard]] Status publish(const ArtifactDescriptor& artifact, std::string_view payload,
                               bool corrupt = false);
  void set_missing(const ArtifactId& artifact);
  void set_resolve_failure(ErrorCode code);

  [[nodiscard]] std::string id() const override { return "synthetic-artifact-source"; }
  [[nodiscard]] Result<ArtifactPayload> resolve(const ArtifactDescriptor& artifact) override;

 private:
  mutable std::mutex mutex_;
  std::string directory_;
  std::map<std::string, std::string> paths_;
  std::map<std::string, bool> missing_;
  bool has_failure_ = false;
  ErrorCode failure_ = ErrorCode::ok;
};

class [[nodiscard]] SyntheticHealthProbe : public HealthProbePort {
 public:
  void set_health(const TargetId& target, bool healthy, std::string detail);
  void set_validity(const TargetId& target, Duration validity);
  void set_age_offset(const TargetId& target, Duration offset);
  void set_foreign_incarnation(const TargetId& target, bool value);
  void set_probe_failure(ErrorCode code);
  void clear();

  [[nodiscard]] std::string id() const override { return "synthetic-health-probe"; }
  [[nodiscard]] Result<HealthReport> probe(const TargetId& target, Timestamp now,
                                           Incarnation incarnation) override;
  [[nodiscard]] std::size_t probes() const;

 private:
  struct Entry {
    bool healthy = true;
    std::string detail = "synthetic health ok";
    Duration validity = Duration::from_seconds(30);
    Duration age_offset;
    bool foreign_incarnation = false;
  };
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  bool has_failure_ = false;
  ErrorCode failure_ = ErrorCode::ok;
  std::size_t probes_ = 0;
};

class [[nodiscard]] SyntheticDrainFabric : public DrainFabricPort {
 public:
  void set_refuse(bool refuse, std::string detail = "maintenance window closed");
  void set_still_drained(bool drained) { still_drained_ = drained; }
  [[nodiscard]] std::string id() const override { return "synthetic-drain-fabric"; }
  [[nodiscard]] Result<DrainTicket> acquire(const DrainRequest& request) override;
  [[nodiscard]] Status release(const DrainTicket& ticket) override;
  [[nodiscard]] Result<bool> still_drained(const DrainTicket& ticket) override;
  [[nodiscard]] std::size_t acquisitions() const;
  [[nodiscard]] std::size_t releases() const;

 private:
  mutable std::mutex mutex_;
  bool refuse_ = false;
  std::string detail_;
  bool still_drained_ = true;
  std::size_t acquisitions_ = 0;
  std::size_t releases_ = 0;
  std::size_t counter_ = 0;
};

class [[nodiscard]] SyntheticRolloutFabric : public RolloutFabricPort {
 public:
  void set_admit(bool admit, std::string detail = "outside the change window");
  [[nodiscard]] std::string id() const override { return "synthetic-rollout-fabric"; }
  [[nodiscard]] Result<RolloutAdmission> admit(const RolloutRequest& request) override;
  [[nodiscard]] Status stage_started(const std::string& change_id, const StageId& stage,
                                     const std::vector<TargetId>& targets, Timestamp at) override;
  [[nodiscard]] Status stage_completed(const std::string& change_id, const StageId& stage,
                                       Timestamp at) override;
  [[nodiscard]] Status release(const std::string& change_id, Timestamp at) override;
  [[nodiscard]] std::size_t admissions() const;

 private:
  mutable std::mutex mutex_;
  bool admit_ = true;
  std::string detail_;
  std::size_t admissions_ = 0;
  std::size_t counter_ = 0;
};

class [[nodiscard]] SyntheticConfigurationFabric : public ConfigurationFabricPort {
 public:
  void set_failure(ErrorCode code);
  [[nodiscard]] std::string id() const override { return "synthetic-configuration-fabric"; }
  [[nodiscard]] Status deliver(const ConfigurationDelivery& delivery) override;
  [[nodiscard]] Result<std::string> current_revision(const TargetId& target) override;
  [[nodiscard]] std::size_t deliveries() const;
  [[nodiscard]] std::vector<std::string> delivered_versions() const;

 private:
  mutable std::mutex mutex_;
  bool has_failure_ = false;
  ErrorCode failure_ = ErrorCode::ok;
  std::size_t deliveries_ = 0;
  std::vector<std::string> versions_;
};

class [[nodiscard]] SyntheticLedger : public ProvenanceLedgerPort {
 public:
  void set_failure(ErrorCode code);
  [[nodiscard]] std::string id() const override { return "synthetic-provenance-ledger"; }
  [[nodiscard]] Status append(const ProvenanceRecord& record) override;
  [[nodiscard]] std::vector<ProvenanceRecord> records() const;

 private:
  mutable std::mutex mutex_;
  std::vector<ProvenanceRecord> records_;
  bool has_failure_ = false;
  ErrorCode failure_ = ErrorCode::ok;
};

}  // namespace fum
