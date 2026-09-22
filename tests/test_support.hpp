// Test support: a small deterministic test framework plus a fully wired
// synthetic harness for the engine.
//
// No test uses a timeout: a hanging test is a defect to diagnose.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "fum/adapters/synthetic_adapter.hpp"
#include "fum/core/fs.hpp"
#include "fum/engine/engine.hpp"

namespace fumtest {

class TestFailure : public std::exception {
 public:
  explicit TestFailure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

[[nodiscard]] inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, TestFunction function) {
    registry().push_back(TestCase{name, function});
  }
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
  std::ostringstream stream;
  stream << file << ":" << line << ": " << message;
  throw TestFailure(stream.str());
}

template <class T>
concept Streamable = requires(std::ostream& stream, const T& value) { stream << value; };

template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (Streamable<T>) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return std::string("<value>");
  }
}

[[nodiscard]] inline std::string describe(fum::ErrorCode code) {
  return fum::error_code_name(code);
}

[[nodiscard]] inline std::string describe(fum::UpgradeState state) {
  return fum::upgrade_state_name(state);
}

[[nodiscard]] inline std::string describe(fum::AttemptState state) {
  return fum::attempt_state_name(state);
}

[[nodiscard]] inline std::string& argv0_storage() {
  static std::string value;
  return value;
}

// Directory of the running test binary: sibling build artifacts live here.
[[nodiscard]] inline std::string executable_directory() {
  const std::string& path = argv0_storage();
  const std::size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

[[nodiscard]] inline int run_all(int argc, char** argv) {
  if (argc > 0 && argv != nullptr) {
    argv0_storage() = argv[0] == nullptr ? std::string() : std::string(argv[0]);
  }
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    }
  }
  std::size_t passed = 0;
  std::vector<std::string> failures;
  for (const auto& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    try {
      test.function();
      ++passed;
      std::cout << "[pass] " << test.name << "\n";
    } catch (const TestFailure& failure) {
      failures.push_back(test.name + ": " + failure.what());
      std::cout << "[FAIL] " << test.name << "\n        " << failure.what() << "\n";
    } catch (const std::exception& error) {
      failures.push_back(test.name + ": unexpected exception: " + error.what());
      std::cout << "[FAIL] " << test.name << " (exception) " << error.what() << "\n";
    }
  }
  std::cout << "\n" << passed << " passed, " << failures.size() << " failed\n";
  for (const auto& failure : failures) {
    std::cout << "  - " << failure << "\n";
  }
  return failures.empty() ? 0 : 1;
}

}  // namespace fumtest

// Constant conditions inside the assertion macros are intentional: several
// checks compare compile-time values.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4127)
#endif

#define FUM_TEST(name)                                                    \
  void name();                                                            \
  namespace {                                                             \
  const ::fumtest::Registrar fum_registrar_##name(#name, &name);          \
  }                                                                       \
  void name()

#define FUM_CHECK(condition)                                                          \
  do {                                                                                \
    const bool fum_condition = static_cast<bool>(condition);                          \
    if (!fum_condition) {                                                             \
      ::fumtest::fail(__FILE__, __LINE__, "check failed: " #condition);               \
    }                                                                                 \
  } while (false)

#define FUM_CHECK_FALSE(condition) FUM_CHECK(!(condition))
#define FUM_CHECK_TRUE(condition) FUM_CHECK(condition)

#define FUM_CHECK_EQ(actual, expected)                                                \
  do {                                                                                \
    const auto fum_actual = (actual);                                                 \
    const auto fum_expected = (expected);                                             \
    const bool fum_equal = (fum_actual == fum_expected);                              \
    if (!fum_equal) {                                                                 \
      ::fumtest::fail(__FILE__, __LINE__,                                             \
                      std::string("expected ") + #actual + " == " + #expected +       \
                          " but got " + ::fumtest::describe(fum_actual) + " vs " +    \
                          ::fumtest::describe(fum_expected));                         \
    }                                                                                 \
  } while (false)

#define FUM_CHECK_NE(actual, expected)                                                \
  do {                                                                                \
    const bool fum_unexpectedly_equal = ((actual) == (expected));                     \
    if (fum_unexpectedly_equal) {                                                     \
      ::fumtest::fail(__FILE__, __LINE__, #actual " unexpectedly equals " #expected); \
    }                                                                                 \
  } while (false)

// Every engine call in a test goes through these helpers so that a failure is
// reported where it happened rather than as an exception.
#define FUM_REQUIRE_OK(expr)                                                          \
  do {                                                                                \
    const auto fum_status = (expr);                                                    \
    if (!fum_status.has_value()) {                                                     \
      ::fumtest::fail(__FILE__, __LINE__,                                              \
                      std::string("expected success from " #expr " but got ") +        \
                          fum_status.error().to_string());                            \
    }                                                                                 \
  } while (false)

#define FUM_REQUIRE_ERR(expr, expected_code)                                          \
  do {                                                                                \
    const auto fum_status = (expr);                                                    \
    if (fum_status.has_value()) {                                                      \
      ::fumtest::fail(__FILE__, __LINE__, #expr " unexpectedly succeeded");            \
    }                                                                                 \
    if (fum_status.error().code() != (expected_code)) {                                \
      ::fumtest::fail(__FILE__, __LINE__,                                              \
                      std::string(#expr " failed with the wrong code: ") +             \
                          fum_status.error().to_string() + " expected " +              \
                          ::fum::error_code_name(expected_code));                      \
    }                                                                                 \
  } while (false)

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#define FUM_REQUIRE_VALUE(dest, expr)                                                 \
  auto fum_value_result = (expr);                                                     \
  if (!fum_value_result.has_value()) {                                                \
    ::fumtest::fail(__FILE__, __LINE__,                                                \
                    std::string("expected a value from " #expr " but got ") +          \
                        fum_value_result.error().to_string());                        \
  }                                                                                   \
  dest = std::move(fum_value_result).value()

namespace fumtest {

using fum::AdapterClaims;
using fum::AdapterId;
using fum::AdapterKind;
using fum::ArtifactDescriptor;
using fum::ArtifactId;
using fum::AttemptState;
using fum::AuthorityId;
using fum::BuildId;
using fum::CampaignId;
using fum::ComponentId;
using fum::ComponentType;
using fum::Digest;
using fum::Duration;
using fum::Engine;
using fum::EngineConfig;
using fum::EngineDependencies;
using fum::ExecutorKind;
using fum::Generation;
using fum::Incarnation;
using fum::ManualClock;
using fum::Policy;
using fum::PreflightReport;
using fum::Revision;
using fum::StageId;
using fum::StartOptions;
using fum::StrategyKind;
using fum::SyntheticAdapter;
using fum::SyntheticArtifactSource;
using fum::SyntheticCompatibilityRegistry;
using fum::SyntheticConfigurationFabric;
using fum::SyntheticDrainFabric;
using fum::SyntheticFault;
using fum::SyntheticHealthProbe;
using fum::SyntheticInventory;
using fum::SyntheticLedger;
using fum::SyntheticRolloutFabric;
using fum::SyntheticTarget;
using fum::TargetDescriptor;
using fum::TargetId;
using fum::Timestamp;
using fum::UpgradePlanRequest;
using fum::UpgradeState;
using fum::Version;

struct [[nodiscard]] HarnessOptions {
  std::size_t target_count = 2;
  ComponentId component = fum::make_ComponentId("fabric-core");
  Version from_version{1, 0, 0};
  Version to_version{1, 1, 0};
  BuildId from_build = fum::make_BuildId("build-1");
  BuildId to_build = fum::make_BuildId("build-2");
  StrategyKind strategy = StrategyKind::restart_based;
  std::string platform = "synthetic/x86_64";
  std::string component_type = "software";
  bool reversible = true;
  bool irreversible_declared = false;
  bool acknowledge_irreversible = true;
  bool registry_compatible = true;
  bool corrupt_artifact = false;
  bool with_health_probe = false;
  bool with_drain = true;
  bool with_rollout = false;
  bool with_configuration = false;
  bool with_ledger = false;
  bool rollback_claim = true;
  bool generation_capable = false;
  bool generation_handoff_claim = false;
  bool requires_service_removal = false;
  std::uint32_t max_parallel = 4;
  ExecutorKind executor = ExecutorKind::deterministic_inline;
  std::size_t worker_threads = 4;
  std::string redundancy_group = "pair-a";
  std::string irreversible_boundary = "storage-format-v3";
  // When set, the harness opens this directory instead of a fresh one. Used to
  // seed durable state left behind by a crashed incarnation.
  std::string state_directory;
};

// Fully wired engine: synthetic fabric, durable store in a temporary directory
// and a manual clock.
class [[nodiscard]] Harness {
 public:
  Harness() = default;
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  [[nodiscard]] static std::unique_ptr<Harness> create(const HarnessOptions& options = {});
  // Reopens the engine over the same state directory (restart / recovery tests).
  [[nodiscard]] fum::Status reopen();

  [[nodiscard]] Engine& engine() { return *engine_; }
  [[nodiscard]] const std::unique_ptr<Engine>& engine_ptr() const { return engine_; }
  [[nodiscard]] SyntheticAdapter& adapter() { return *adapter_; }
  [[nodiscard]] SyntheticCompatibilityRegistry& registry() { return *registry_; }
  [[nodiscard]] SyntheticArtifactSource& artifacts() { return *artifacts_; }
  [[nodiscard]] SyntheticInventory& inventory() { return *inventory_; }
  [[nodiscard]] SyntheticHealthProbe& health() { return *health_; }
  [[nodiscard]] SyntheticDrainFabric& drain() { return *drain_; }
  [[nodiscard]] SyntheticRolloutFabric& rollout() { return *rollout_; }
  [[nodiscard]] SyntheticConfigurationFabric& configuration() { return *configuration_; }
  [[nodiscard]] SyntheticLedger& ledger() { return *ledger_; }
  [[nodiscard]] ManualClock& clock() { return *clock_; }
  [[nodiscard]] const HarnessOptions& options() const { return options_; }
  [[nodiscard]] const ArtifactDescriptor& artifact() const { return artifact_; }
  [[nodiscard]] const std::string& payload() const { return payload_; }
  [[nodiscard]] const std::string& state_directory() const { return state_directory_; }
  [[nodiscard]] const std::vector<TargetId>& targets() const { return target_ids_; }
  [[nodiscard]] const TargetId& target(std::size_t index) const { return target_ids_.at(index); }
  [[nodiscard]] const CampaignId& campaign() const { return campaign_; }

  [[nodiscard]] UpgradePlanRequest request() const;
  [[nodiscard]] fum::Result<fum::UpgradePlan> plan();
  [[nodiscard]] CampaignId create_campaign();
  [[nodiscard]] fum::Result<PreflightReport> preflight();
  [[nodiscard]] fum::Status start(const StartOptions& options = {});
  [[nodiscard]] fum::Status run();
  [[nodiscard]] fum::Result<fum::CampaignStatus> status();
  [[nodiscard]] UpgradeState state();
  [[nodiscard]] std::vector<fum::AttemptRecord> attempts();
  [[nodiscard]] std::size_t provenance_count();
  void advance(Duration delta) { clock_->advance(delta); }

 private:
  HarnessOptions options_;
  std::string state_directory_;
  std::string artifact_directory_;
  std::unique_ptr<fum::fs::TempDir> state_guard_;
  ArtifactDescriptor artifact_;
  std::string payload_;
  CampaignId campaign_;
  std::vector<TargetId> target_ids_;
  std::shared_ptr<ManualClock> clock_;
  std::shared_ptr<SyntheticAdapter> adapter_;
  std::shared_ptr<SyntheticInventory> inventory_;
  std::shared_ptr<SyntheticCompatibilityRegistry> registry_;
  std::shared_ptr<SyntheticArtifactSource> artifacts_;
  std::shared_ptr<SyntheticHealthProbe> health_;
  std::shared_ptr<SyntheticDrainFabric> drain_;
  std::shared_ptr<SyntheticRolloutFabric> rollout_;
  std::shared_ptr<SyntheticConfigurationFabric> configuration_;
  std::shared_ptr<SyntheticLedger> ledger_;
  EngineDependencies dependencies_;
  EngineConfig config_;
  std::unique_ptr<Engine> engine_;
};

[[nodiscard]] inline std::string unique_suffix() {
  static std::atomic<std::uint64_t> counter{0};
  return std::to_string(static_cast<unsigned long long>(::fum::fs::process_id_string().size())) + "-" +
         std::to_string(counter.fetch_add(1));
}

inline std::unique_ptr<Harness> Harness::create(const HarnessOptions& options) {
  auto harness = std::make_unique<Harness>();
  harness->options_ = options;
  if (!options.state_directory.empty()) {
    harness->state_directory_ = options.state_directory;
  } else {
    auto state_directory = fum::fs::TempDir::create("fum-test-state");
    if (!state_directory.has_value()) {
      fail(__FILE__, __LINE__, "could not create a temporary state directory");
    }
    harness->state_guard_ =
        std::make_unique<fum::fs::TempDir>(std::move(state_directory).value());
    harness->state_guard_->keep();
    harness->state_directory_ = harness->state_guard_->path();
  }

  harness->clock_ = std::make_shared<ManualClock>();

  // Artifact + payload.
  const std::string artifact_id = "artifact-" + options.component.str() + "-" +
                                  options.to_version.text();
  harness->payload_ = "fum-artifact-payload:" + artifact_id + ":" + options.to_build.str();
  harness->artifact_.set_id(fum::make_ArtifactId(artifact_id));
  harness->artifact_.set_component(options.component);
  harness->artifact_.set_version(options.to_version);
  harness->artifact_.set_build(options.to_build);
  harness->artifact_.set_component_type(options.component_type == "firmware"
                                            ? ComponentType::firmware
                                            : ComponentType::software);
  harness->artifact_.set_digest(Digest::of_bytes(harness->payload_));
  harness->artifact_.set_size_bytes(static_cast<std::uint64_t>(harness->payload_.size()));
  harness->artifact_.set_platforms({options.platform});
  harness->artifact_.set_media_type("application/octet-stream");
  harness->artifact_.set_created_at(harness->clock_->now());
  fum::SignerProvenance provenance;
  provenance.signer = fum::make_SignerId("summon-software-labs");
  provenance.key_id = "key-1";
  provenance.algorithm = "ed25519";
  provenance.source_revision = "rev-1";
  provenance.build_system = "cmake";
  harness->artifact_.set_provenance(provenance);
  fum::RollbackCompatibility rollback;
  rollback.reversible = options.reversible;
  rollback.boundary =
      (options.reversible && !options.irreversible_declared) ? std::string()
                                                             : options.irreversible_boundary;
  if (options.irreversible_declared) {
    rollback.irreversible_steps.push_back(fum::make_StepId("activate"));
  }
  harness->artifact_.set_rollback(rollback);

  // Adapter.
  harness->adapter_ = std::make_shared<SyntheticAdapter>(fum::make_AdapterId("synthetic-adapter"));
  AdapterClaims claims;
  claims.strategies = {StrategyKind::in_place, StrategyKind::restart_based,
                       StrategyKind::redundant_pair_rolling};
  if (options.generation_handoff_claim) {
    claims.strategies.push_back(StrategyKind::control_plane_generation_handoff);
  }
  claims.supports_prepare = true;
  claims.supports_rollback = options.rollback_claim;
  claims.supports_health_probe = true;
  claims.supports_version_observation = true;
  claims.supports_generation_handoff = options.generation_handoff_claim;
  claims.requires_service_removal = options.requires_service_removal;
  claims.max_parallel_activations = options.max_parallel;
  harness->adapter_->set_claims(claims);

  harness->inventory_ = std::make_shared<SyntheticInventory>();
  for (std::size_t i = 0; i < options.target_count; ++i) {
    const std::string id = "target-" + std::to_string(i + 1);
    SyntheticTarget synthetic;
    synthetic.id = fum::make_TargetId(id);
    synthetic.component = options.component;
    synthetic.display_name = id;
    synthetic.version = options.from_version;
    synthetic.build = options.from_build;
    synthetic.platform = options.platform;
    synthetic.capabilities = {"hot-restart"};
    synthetic.authority = fum::make_AuthorityId("fabric-authority");
    synthetic.generation_capable = options.generation_capable;
    synthetic.redundancy_group = options.redundancy_group;
    harness->adapter_->add_target(synthetic);
    harness->target_ids_.push_back(synthetic.id);

    TargetDescriptor descriptor;
    descriptor.id = synthetic.id;
    descriptor.component = options.component;
    descriptor.display_name = synthetic.display_name;
    descriptor.installed_version = options.from_version;
    descriptor.installed_build = options.from_build;
    descriptor.platform = options.platform;
    descriptor.capabilities = synthetic.capabilities;
    descriptor.adapter = harness->adapter_->id();
    descriptor.adapter_kind = AdapterKind::synthetic;
    descriptor.claims = claims;
    descriptor.authority = synthetic.authority;
    descriptor.redundancy_group = options.redundancy_group;
    descriptor.generation_capable = options.generation_capable;
    harness->inventory_->add_target(descriptor);
  }

  harness->registry_ = std::make_shared<SyntheticCompatibilityRegistry>("registry-rev-1");
  fum::CompatibilityMatrixEntry entry;
  entry.component = options.component;
  entry.from = options.from_version;
  entry.to = options.to_version;
  entry.compatible = options.registry_compatible;
  entry.reason = options.registry_compatible ? "" : "declared incompatible by the registry export";
  entry.rollback_supported = options.reversible;
  entry.rollback_boundary = options.irreversible_boundary;
  harness->registry_->add_entry(entry);

  harness->artifacts_ = std::make_shared<SyntheticArtifactSource>();
  FUM_REQUIRE_OK(harness->artifacts_->publish(harness->artifact_, harness->payload_,
                                              options.corrupt_artifact));

  harness->health_ = std::make_shared<SyntheticHealthProbe>();
  harness->drain_ = std::make_shared<SyntheticDrainFabric>();
  harness->rollout_ = std::make_shared<SyntheticRolloutFabric>();
  harness->configuration_ = std::make_shared<SyntheticConfigurationFabric>();
  harness->ledger_ = std::make_shared<SyntheticLedger>();

  harness->config_.store.directory = harness->state_directory_;
  harness->config_.policy.id = fum::make_PolicyId("test-policy");
  harness->config_.policy.revision = Revision(1);
  harness->config_.policy.max_attempts_per_target = 3;
  harness->config_.policy.max_retries_per_operation = 2;
  harness->config_.policy.require_health_gate = true;
  harness->config_.executor = options.executor;
  harness->config_.worker_threads = options.worker_threads;
  harness->config_.store.journal.sync_on_append = true;

  harness->dependencies_.inventory = harness->inventory_;
  harness->dependencies_.compatibility = harness->registry_;
  harness->dependencies_.artifacts = harness->artifacts_;
  harness->dependencies_.clock = harness->clock_;
  harness->dependencies_.adapters.add(harness->adapter_);
  if (options.with_health_probe) {
    harness->dependencies_.health = harness->health_;
  }
  if (options.with_drain) {
    harness->dependencies_.drain = harness->drain_;
  }
  if (options.with_rollout) {
    harness->dependencies_.rollout = harness->rollout_;
  }
  if (options.with_configuration) {
    harness->dependencies_.configuration = harness->configuration_;
  }
  if (options.with_ledger) {
    harness->dependencies_.ledger = harness->ledger_;
  }

  harness->campaign_ = fum::make_CampaignId("campaign-" + options.component.str() + "-" +
                                            options.to_version.text());
  FUM_REQUIRE_OK(harness->reopen());
  return harness;
}

inline fum::Status Harness::reopen() {
  if (engine_) {
    static_cast<void>(engine_->shutdown());
    engine_.reset();
  }
  auto opened = Engine::open(config_, dependencies_);
  if (!opened.has_value()) {
    return opened.error();
  }
  engine_ = std::move(opened).value();
  return fum::ok_status();
}

inline UpgradePlanRequest Harness::request() const {
  UpgradePlanRequest request;
  request.campaign = campaign_;
  request.component = options_.component;
  request.artifact = artifact_;
  request.strategy = options_.strategy;
  request.authority = fum::make_AuthorityId("fabric-authority");
  request.acknowledge_irreversible_steps = options_.acknowledge_irreversible;
  request.max_parallel_activations = options_.max_parallel;
  request.description = "test upgrade";
  request.requested_at = clock_->now();
  fum::SkewBudget budget;
  budget.max_major_skew = 1;
  budget.max_minor_skew = 2;
  budget.max_patch_skew = 16;
  request.skew_budget = budget;
  return request;
}

inline fum::Result<fum::UpgradePlan> Harness::plan() { return engine_->plan(request()); }

inline CampaignId Harness::create_campaign() {
  auto created = engine_->create_campaign(request());
  if (!created.has_value()) {
    fail(__FILE__, __LINE__, "create_campaign failed: " + created.error().to_string());
  }
  return created.value().campaign;
}

inline fum::Result<PreflightReport> Harness::preflight() { return engine_->preflight(campaign_); }

inline fum::Status Harness::start(const StartOptions& options) {
  return engine_->start(campaign_, options);
}

inline fum::Status Harness::run() { return engine_->run_until_idle(); }

inline fum::Result<fum::CampaignStatus> Harness::status() { return engine_->status(campaign_); }

inline UpgradeState Harness::state() {
  auto current = status();
  if (!current.has_value()) {
    fail(__FILE__, __LINE__, "status failed: " + current.error().to_string());
  }
  return current.value().campaign.state;
}

inline std::vector<fum::AttemptRecord> Harness::attempts() {
  auto current = status();
  if (!current.has_value()) {
    fail(__FILE__, __LINE__, "status failed: " + current.error().to_string());
  }
  return current.value().attempts;
}

inline std::size_t Harness::provenance_count() {
  auto records = engine_->provenance(fum::CampaignId{});
  if (!records.has_value()) {
    fail(__FILE__, __LINE__, "provenance failed: " + records.error().to_string());
  }
  return records.value().size();
}

// Convenience: create, preflight, start and run a campaign in one call.
inline void run_to_completion(Harness& harness, const StartOptions& options = {}) {
  static_cast<void>(harness.create_campaign());
  auto report = harness.preflight();
  if (!report.has_value()) {
    fail(__FILE__, __LINE__, "preflight failed: " + report.error().to_string());
  }
  if (!report.value().passed) {
    fail(__FILE__, __LINE__,
         "preflight did not pass: " +
             (report.value().blockers.empty() ? std::string("no blockers reported")
                                              : report.value().blockers.front()));
  }
  auto started = harness.start(options);
  if (!started.has_value()) {
    fail(__FILE__, __LINE__, "start failed: " + started.error().to_string());
  }
  auto ran = harness.run();
  if (!ran.has_value()) {
    fail(__FILE__, __LINE__, "run failed: " + ran.error().to_string());
  }
}

}  // namespace fumtest
