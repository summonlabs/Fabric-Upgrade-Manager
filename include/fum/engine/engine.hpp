// Fabric Upgrade Manager engine.
//
// Governance (planning, preflight, lifecycle, fencing, verification, provenance)
// lives here. Vendor-specific installation mechanics live behind
// TargetAdapterPort; compatibility knowledge, rollout sequencing, service
// removal and configuration delivery live behind their own ports.
//
// Locking contract (audited, see README "Concurrency audit"):
//   * exactly one engine mutex guards durable state;
//   * the mutex is never held while calling an adapter, a port, the logger or
//     the worker pool;
//   * log records are collected under the lock and published after it is
//     released;
//   * no callback into user code is invoked while the mutex is held.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fum/core/log.hpp"
#include "fum/engine/preflight.hpp"
#include "fum/engine/reconcile.hpp"
#include "fum/engine/verification.hpp"
#include "fum/ports/ports.hpp"
#include "fum/runtime/worker_pool.hpp"
#include "fum/store/state.hpp"

namespace fum {

enum class ExecutorKind : std::uint8_t { deterministic_inline = 0, thread_pool };

[[nodiscard]] const char* executor_kind_name(ExecutorKind kind) noexcept;
[[nodiscard]] Result<ExecutorKind> parse_executor_kind(std::string_view text);

// Inventory resolution. Fabric Upgrade Manager does not own a CMDB: targets are
// resolved from the fabric inventory source.
class InventorySourcePort {
 public:
  InventorySourcePort() = default;
  InventorySourcePort(const InventorySourcePort&) = delete;
  InventorySourcePort& operator=(const InventorySourcePort&) = delete;
  virtual ~InventorySourcePort();
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual Result<std::vector<TargetDescriptor>> resolve(Timestamp now,
                                                                    Incarnation incarnation) = 0;
};

struct [[nodiscard]] EngineDependencies {
  std::shared_ptr<InventorySourcePort> inventory;
  std::shared_ptr<CompatibilityRegistryPort> compatibility;
  std::shared_ptr<RolloutFabricPort> rollout;
  std::shared_ptr<DrainFabricPort> drain;
  std::shared_ptr<ConfigurationFabricPort> configuration;
  std::shared_ptr<HealthProbePort> health;
  std::shared_ptr<ArtifactSourcePort> artifacts;
  std::shared_ptr<ProvenanceLedgerPort> ledger;
  AdapterRegistry adapters;
  std::shared_ptr<Clock> clock;
};

struct [[nodiscard]] EngineConfig {
  StoreOptions store;
  Policy policy;
  ExecutorKind executor = ExecutorKind::deterministic_inline;
  std::size_t worker_threads = 4;
  std::size_t max_queue = 256;
  std::size_t max_work_items_per_pump = 64;
  std::size_t max_pump_iterations = 100000;
  bool require_health_gate = true;
  Logger* logger = nullptr;   // borrowed; may be null
};

struct [[nodiscard]] StartOptions {
  bool dry_run = false;
  bool allow_irreversible = false;
  std::uint32_t max_parallel = 0;   // 0 = policy default
  std::string reason;
};

enum class OperationKind : std::uint8_t { prepare = 0, activate = 1, verify = 2, rollback = 3 };

[[nodiscard]] const char* operation_kind_name(OperationKind kind) noexcept;

struct [[nodiscard]] WorkItem {
  OperationKind kind = OperationKind::prepare;
  CampaignId campaign;
  Generation generation;
  Epoch epoch;
  StageId stage;
  TargetId target;
  AttemptId attempt;
  ArtifactDescriptor artifact;
  TargetDescriptor target_descriptor;
  StrategyKind strategy = StrategyKind::in_place;
  std::string payload_path;
  std::string rollout_change;
  bool requires_service_removal = false;
};

// Everything an adapter reported for one operation, plus the health evidence
// the engine gathered around it.
struct [[nodiscard]] OperationOutcome {
  OperationKind kind = OperationKind::prepare;
  PrepareOutcome prepare;
  ActivateOutcome activate;
  VerifyOutcome verify;
  RollbackOutcome rollback;
  HealthReport health;
  bool has_health = false;
  bool skipped_because_applied = false;
  bool gate_refused = false;
  std::string gate_reason;

  [[nodiscard]] json::Value to_json() const;
};

struct [[nodiscard]] CampaignStatus {
  CampaignRecord campaign;
  std::vector<AttemptRecord> attempts;
  std::size_t in_flight = 0;
  bool ticket_current = false;
  std::string ticket_detail;
  SkewAssessment skew;
  std::vector<Decision> decisions;

  [[nodiscard]] json::Value to_json() const;
};

class [[nodiscard]] Engine {
 public:
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine();

  [[nodiscard]] static Result<std::unique_ptr<Engine>> open(const EngineConfig& config,
                                                            EngineDependencies dependencies);

  // ---- commands -----------------------------------------------------------
  [[nodiscard]] Result<UpgradePlan> plan(const UpgradePlanRequest& request);
  [[nodiscard]] Result<UpgradePlan> create_campaign(const UpgradePlanRequest& request);
  [[nodiscard]] Result<PreflightReport> preflight(const CampaignId& campaign);
  [[nodiscard]] Status start(const CampaignId& campaign, const StartOptions& options = {});
  [[nodiscard]] Status pause(const CampaignId& campaign, std::string reason);
  [[nodiscard]] Status resume(const CampaignId& campaign);
  [[nodiscard]] Status abort(const CampaignId& campaign, std::string reason);
  [[nodiscard]] Result<PreflightReport> rollback(const CampaignId& campaign, bool execute);

  // ---- queries ------------------------------------------------------------
  [[nodiscard]] Result<CampaignStatus> status(const CampaignId& campaign) const;
  [[nodiscard]] Result<std::vector<CampaignStatus>> campaigns() const;
  [[nodiscard]] Result<std::string> explain(const CampaignId& campaign) const;
  [[nodiscard]] Result<InventorySnapshot> inventory();
  [[nodiscard]] Result<std::vector<ProvenanceRecord>> provenance(const CampaignId& campaign) const;
  [[nodiscard]] Result<std::vector<Decision>> decisions(const CampaignId& campaign) const;
  [[nodiscard]] ReconcileReport reconciliation() const;
  [[nodiscard]] json::Value stats() const;
  [[nodiscard]] Incarnation incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] Epoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] const Policy& policy() const noexcept { return policy_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return store_.recovery(); }
  [[nodiscard]] const std::string& state_directory() const noexcept {
    return config_.store.directory;
  }

  // ---- execution drivers ---------------------------------------------------
  // Runs every ready work item on the calling thread until nothing is ready.
  [[nodiscard]] Status run_until_idle();
  // Runs at most max_steps driver iterations and returns; reaching the bound is
  // not an error for this bounded API (run_until_idle reports it as one).
  [[nodiscard]] Status run_steps(std::size_t max_steps);
  // Waits until a pool-driven run has finished. Never uses a timeout.
  [[nodiscard]] Status wait_for_idle();
  [[nodiscard]] Status shutdown();
  [[nodiscard]] bool is_idle() const;

 private:
  Engine(EngineConfig config, EngineDependencies dependencies);

  [[nodiscard]] Status initialize();
  [[nodiscard]] Status apply_reconciliation(Timestamp now);
  [[nodiscard]] Status refresh_inventory(Timestamp now, bool force);
  [[nodiscard]] Result<InventorySnapshot> inventory_locked() const;
  [[nodiscard]] Result<IntegrityAssessment> resolve_artifact(const ArtifactDescriptor& artifact,
                                                             std::string& payload_path);
  [[nodiscard]] Result<std::map<std::string, CompatibilityVerdict>> query_compatibility(
      const UpgradePlan& plan, const InventorySnapshot& inventory, Timestamp now);

  void plan_work_locked(std::vector<WorkItem>& out, std::size_t budget);
  [[nodiscard]] Status execute_item(const WorkItem& item);
  [[nodiscard]] Status commit_item(const WorkItem& item, const OperationOutcome& outcome);
  [[nodiscard]] Status commit_locked(const WorkItem& item, const OperationOutcome& outcome,
                                     std::vector<std::pair<LogLevel, std::string>>& messages);
  void release_finished_drains();
  [[nodiscard]] Status run_steps_bounded(std::size_t max_steps, bool report_exhaustion);
  // Pool driver loop: keeps running while new campaigns are admitted.
  void driver_loop();
  [[nodiscard]] bool has_in_flight_locked() const;
  [[nodiscard]] Status check_fence_locked(const WorkItem& item,
                                          AttemptState expected) const;

  [[nodiscard]] Status transition_locked(CampaignRecord& campaign, UpgradeState to,
                                         const TransitionGuards& guards);
  [[nodiscard]] TransitionGuards guards_locked(const CampaignRecord& campaign);
  [[nodiscard]] bool ticket_current_locked(const CampaignRecord& campaign, Timestamp now,
                                           std::string& detail) const;
  void record_decision_locked(Decision decision);
  [[nodiscard]] Decision make_decision_locked(DecisionKind kind, const CampaignRecord& campaign,
                                              DecisionOutcome outcome, std::string selected,
                                              std::string rationale,
                                              std::vector<DecisionInput> inputs = {},
                                              std::vector<RejectedAlternative> rejected = {},
                                              std::vector<DecisionEvidenceRef> evidence = {}) const;
  [[nodiscard]] Status append_provenance_locked(const WorkItem& item, ProvenanceOutcome outcome,
                                                std::string detail, Timestamp now);
  [[nodiscard]] AttemptId allocate_attempt_locked(const CampaignRecord& campaign,
                                                  const StageId& stage, const TargetId& target);
  [[nodiscard]] Status fail_locked(CampaignRecord& campaign, const std::string& reason);
  [[nodiscard]] SkewAssessment assess_current_skew_locked(const CampaignRecord& campaign) const;
  void log(LogLevel level, std::string message, std::vector<LogField> fields) const;

  EngineConfig config_;
  EngineDependencies deps_;
  Policy policy_;
  DurableStore store_;
  mutable std::mutex mutex_;
  std::condition_variable idle_cv_;
  std::unique_ptr<WorkerPool> pool_;
  Incarnation incarnation_{1};
  Epoch epoch_{1};
  Sequence attempt_sequence_;
  Sequence decision_sequence_;
  std::map<std::string, DrainTicket> drain_tickets_;
  std::map<std::string, std::string> rollout_changes_;
  std::atomic<bool> shutting_down_{false};
  // Incremented on every admitted execution; the driver re-checks it before it
  // goes idle so an admission can never be lost between passes.
  std::atomic<std::uint64_t> admissions_{0};
  bool driver_active_ = false;
  bool pool_mode_ = false;
  InventorySnapshot last_inventory_;
  std::string artifact_payload_path_;
  IntegrityAssessment last_integrity_;
  ReconcileReport reconciliation_;
};

}  // namespace fum
