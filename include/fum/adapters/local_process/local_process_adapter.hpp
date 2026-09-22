// Real local-process upgrade adapter.
//
// This adapter manages an actual control-plane executable: it stages the
// artifact payload into a build slot, stops the running process, atomically
// switches the slot pointer, restarts the process and confirms the new version
// over a real framed socket. It claims restart-based and in-place strategies
// only; it does not claim redundant-pair rolling or generation handoff, and it
// makes no hardware claim of any kind.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fum/adapters/local_process/framed_transport.hpp"
#include "fum/core/log.hpp"
#include "fum/adapters/local_process/process_supervisor.hpp"
#include "fum/ports/ports.hpp"

namespace fum {

struct [[nodiscard]] LocalProcessOptions {
  std::string control_plane_executable;   // the real executable that gets installed
  std::string token = "local-process-token";
  Duration readiness_bound = Duration::from_seconds(20);
  Duration shutdown_grace = Duration::from_seconds(5);
  bool keep_running_after_operations = true;
};

struct [[nodiscard]] LocalProcessTarget {
  TargetId id;
  ComponentId component;
  std::string display_name;
  Version version;
  BuildId build;
  std::string platform = "windows/x86_64";
  std::vector<std::string> capabilities;
  AuthorityId authority;
  std::string root_directory;   // where slots, state and logs live
  bool healthy = true;
  bool generation_capable = false;
  std::string redundancy_group;
};

// Behaviour knobs written into the staged package descriptor.
struct [[nodiscard]] LocalProcessBehavior {
  bool healthy = true;
  std::int64_t startup_delay_ms = 0;
  bool crash_on_start = false;
};

class [[nodiscard]] LocalProcessAdapter : public TargetAdapterPort {
 public:
  LocalProcessAdapter(AdapterId id, LocalProcessOptions options);
  ~LocalProcessAdapter() override;

  void add_target(LocalProcessTarget target);
  void set_behavior(const TargetId& target, LocalProcessBehavior behavior);
  void set_claims(AdapterClaims claims);

  // Fault injection with real effects.
  void drop_next_activate_acknowledgement(const TargetId& target);
  void drop_next_prepare_acknowledgement(const TargetId& target);
  void set_verify_stale(const TargetId& target, bool stale);
  void set_verify_wrong_incarnation(const TargetId& target, bool value);
  [[nodiscard]] Status kill_target_process(const TargetId& target);
  [[nodiscard]] Status restart_target_process(const TargetId& target);
  [[nodiscard]] Status crash_target_process(const TargetId& target);

  [[nodiscard]] AdapterId id() const override { return id_; }
  [[nodiscard]] AdapterKind kind() const override { return AdapterKind::local_process; }
  [[nodiscard]] const AdapterClaims& claims() const override { return claims_; }
  [[nodiscard]] bool manages_processes() const override { return true; }

  [[nodiscard]] Result<TargetObservation> observe(const TargetId& target, Timestamp now,
                                                  Incarnation incarnation) override;
  [[nodiscard]] Result<PrepareOutcome> prepare(const TargetOperation& operation) override;
  [[nodiscard]] Result<ActivateOutcome> activate(const TargetOperation& operation) override;
  [[nodiscard]] Result<VerifyOutcome> verify(const TargetOperation& operation) override;
  [[nodiscard]] Result<RollbackOutcome> rollback(const TargetOperation& operation) override;
  [[nodiscard]] Result<bool> process_alive(const TargetId& target) override;

  // Inspection used by independent-process tests.
  struct RuntimeFacts {
    std::uint64_t pid = 0;
    Incarnation incarnation;
    Version version;
    BuildId build;
    bool alive = false;
    std::uint16_t port = 0;
    std::string current_build;
    std::string detail;
  };
  [[nodiscard]] Result<RuntimeFacts> runtime_facts(const TargetId& target);
  [[nodiscard]] std::size_t restarts(const TargetId& target) const;
  [[nodiscard]] std::string slot_path(const TargetId& target, const BuildId& build) const;

 private:
  struct TargetState {
    LocalProcessTarget target;
    LocalProcessOptions options;
    LocalProcessBehavior behavior;
    proc::ProcessSupervisor supervisor;
    std::uint16_t port = 0;
    Incarnation process_incarnation;
    std::size_t restarts = 0;
    Sequence request_sequence;
    bool drop_next_activate_ack = false;
    bool drop_next_prepare_ack = false;
    bool verify_stale = false;
    bool verify_wrong_incarnation = false;
    std::string current_build;
    std::string previous_build;
    bool staged_ready = false;
    BuildId staged_build;
    Version staged_version;
    std::mutex mutex;
  };

  [[nodiscard]] TargetState* find(const TargetId& target);
  [[nodiscard]] Status ensure_link(TargetState& state, Timestamp now);
  // Every command uses a fresh connection: the control plane serves connections
  // in turn, so caching one would starve concurrent observers.
  [[nodiscard]] Status stop_process(TargetState& state);
  [[nodiscard]] Result<json::Value> command(TargetState& state, const std::string& command);
  [[nodiscard]] Status start_process(TargetState& state, const std::string& build);
  [[nodiscard]] Result<Version> installed_version(const TargetState& state) const;
  [[nodiscard]] std::string write_package(TargetState& state, const ArtifactDescriptor& artifact,
                                          std::string_view payload) const;
  void log_message(LogLevel level, const std::string& message) const;

  AdapterId id_;
  LocalProcessOptions options_;
  AdapterClaims claims_;
  std::map<std::string, std::unique_ptr<TargetState>> targets_;
  mutable std::mutex map_mutex_;
  Logger* logger_ = nullptr;
};

}  // namespace fum
