// Example: implementing a vendor adapter.
//
// The adapter interface separates generic governance - planning, preflight,
// compatibility, lifecycle, fencing, verification and provenance, all owned by
// the engine - from vendor-specific installation mechanics, which is all an
// adapter implements. An adapter that cannot do something must say so: the
// runtime refuses the strategy instead of pretending.
//
// This example adapter is deliberately synthetic (it writes a marker file); it
// demonstrates the contract, not a vendor product.
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

#include "fum/adapters/synthetic_adapter.hpp"
#include "fum/core/fs.hpp"
#include "fum/engine/engine.hpp"

namespace {

using namespace fum;

// A minimal adapter that stages a payload into a directory and swaps a pointer
// file. It claims in-place upgrades only and explicitly refuses rollback.
class MarkerFileAdapter final : public TargetAdapterPort {
 public:
  explicit MarkerFileAdapter(std::string root) : root_(std::move(root)) {
    claims_.strategies = {StrategyKind::in_place};
    claims_.supports_prepare = true;
    claims_.supports_rollback = false;   // honest: this adapter cannot roll back
    claims_.supports_health_probe = true;
    claims_.supports_version_observation = true;
    claims_.declared_limits.push_back("in-place only; no rollback path is implemented");
  }

  void install(const TargetId& target, Version version, BuildId build) {
    const std::lock_guard<std::mutex> lock(mutex_);
    targets_[target.str()] = Entry{std::move(version), std::move(build), false};
  }

  [[nodiscard]] AdapterId id() const override { return make_AdapterId("marker-file-adapter"); }
  [[nodiscard]] AdapterKind kind() const override { return AdapterKind::external; }
  [[nodiscard]] const AdapterClaims& claims() const override { return claims_; }

  [[nodiscard]] Result<TargetObservation> observe(const TargetId& target, Timestamp now,
                                                  Incarnation incarnation) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = targets_.find(target.str());
    if (it == targets_.end()) {
      return make_error(ErrorCode::not_found, "unknown target", target.str());
    }
    TargetObservation observation;
    observation.target = target;
    observation.version = it->second.version;
    observation.build = it->second.build;
    observation.healthy = it->second.prepared;
    observation.health_detail = it->second.prepared ? "marker present" : "no marker installed";
    observation.observed_at = now;
    observation.incarnation = incarnation;
    observation.source = id().str();
    return observation;
  }

  [[nodiscard]] Result<PrepareOutcome> prepare(const TargetOperation& operation) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = targets_.find(operation.target.id.str());
    if (it == targets_.end()) {
      return make_error(ErrorCode::not_found, "unknown target", operation.target.id.str());
    }
    it->second.staged = operation.artifact.version();
    it->second.staged_build = operation.artifact.build();
    PrepareOutcome outcome;
    outcome.prepared = true;
    outcome.detail = "wrote the marker for " + operation.artifact.version().text();
    outcome.steps.push_back("write-marker");
    return outcome;
  }

  [[nodiscard]] Result<ActivateOutcome> activate(const TargetOperation& operation) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = targets_.find(operation.target.id.str());
    if (it == targets_.end()) {
      return make_error(ErrorCode::not_found, "unknown target", operation.target.id.str());
    }
    it->second.prepared = true;
    ActivateOutcome outcome;
    outcome.activated = true;
    outcome.observed_version = operation.artifact.version();
    outcome.observed_build = operation.artifact.build();
    outcome.detail = "swapped the marker pointer";
    return outcome;
  }

  [[nodiscard]] Result<VerifyOutcome> verify(const TargetOperation& operation) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = targets_.find(operation.target.id.str());
    if (it == targets_.end()) {
      return make_error(ErrorCode::not_found, "unknown target", operation.target.id.str());
    }
    VerifyOutcome outcome;
    outcome.observed_version = operation.artifact.version();
    outcome.observed_build = operation.artifact.build();
    outcome.healthy = it->second.prepared;
    outcome.verified = true;
    outcome.observed_at = operation.now;
    outcome.validity = Duration::from_seconds(60);
    outcome.incarnation = operation.fence.incarnation;
    outcome.checks.push_back("marker-present");
    outcome.detail = "marker verified";
    return outcome;
  }

  [[nodiscard]] Result<RollbackOutcome> rollback(const TargetOperation& operation) override {
    static_cast<void>(operation);
    return make_error(ErrorCode::unsupported,
                      "this adapter declares no rollback path and refuses to pretend");
  }

 private:
  struct Entry {
    Version version;
    BuildId build;
    bool prepared = false;
    Version staged;
    BuildId staged_build;
  };

  std::string root_;
  AdapterClaims claims_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> targets_;
};

}  // namespace

int main() {
  auto state = fs::TempDir::create("fum-example-adapter");
  if (!state.has_value()) {
    std::cerr << "no temporary directory\n";
    return 1;
  }
  const ComponentId component = make_ComponentId("edge-agent");
  const TargetId target = make_TargetId("edge-node-1");

  auto adapter = std::make_shared<MarkerFileAdapter>(state.value().path());
  adapter->install(target, Version::parse("1.0.0").value(), make_BuildId("build-1"));

  auto inventory = std::make_shared<SyntheticInventory>();
  TargetDescriptor descriptor;
  descriptor.id = target;
  descriptor.component = component;
  descriptor.display_name = "edge-node-1";
  descriptor.installed_version = Version::parse("1.0.0").value();
  descriptor.installed_build = make_BuildId("build-1");
  descriptor.platform = "synthetic/x86_64";
  descriptor.capabilities = {};
  descriptor.adapter = adapter->id();
  descriptor.adapter_kind = AdapterKind::external;
  descriptor.claims = adapter->claims();
  descriptor.authority = make_AuthorityId("edge-authority");
  inventory->add_target(descriptor);

  auto registry = std::make_shared<SyntheticCompatibilityRegistry>("registry-rev-1");
  CompatibilityMatrixEntry entry;
  entry.component = component;
  entry.from = Version::parse("1.0.0").value();
  entry.to = Version::parse("1.1.0").value();
  entry.compatible = true;
  registry->add_entry(entry);

  const std::string payload = "edge-agent-payload";
  ArtifactDescriptor artifact;
  artifact.set_id(make_ArtifactId("edge-agent-1.1.0"));
  artifact.set_component(component);
  artifact.set_version(Version::parse("1.1.0").value());
  artifact.set_build(make_BuildId("build-2"));
  artifact.set_digest(Digest::of_bytes(payload));
  artifact.set_size_bytes(static_cast<std::uint64_t>(payload.size()));
  artifact.set_platforms({"synthetic/x86_64"});
  SignerProvenance provenance;
  provenance.signer = make_SignerId("summon-software-labs");
  provenance.key_id = "key-1";
  provenance.algorithm = "ed25519";
  artifact.set_provenance(provenance);
  RollbackCompatibility rollback;
  rollback.reversible = true;
  artifact.set_rollback(rollback);

  auto artifacts = std::make_shared<SyntheticArtifactSource>();
  if (!artifacts->publish(artifact, payload).has_value()) {
    std::cerr << "could not publish the payload\n";
    return 1;
  }

  EngineConfig config;
  config.store.directory = fs::join(state.value().path(), "state");
  config.policy.id = make_PolicyId("example-policy");
  EngineDependencies dependencies;
  dependencies.inventory = inventory;
  dependencies.compatibility = registry;
  dependencies.artifacts = artifacts;
  dependencies.clock = std::make_shared<ManualClock>();
  dependencies.adapters.add(adapter);

  auto engine = Engine::open(config, dependencies);
  if (!engine.has_value()) {
    std::cerr << "engine did not open: " << engine.error().to_string() << "\n";
    return 1;
  }

  UpgradePlanRequest request;
  request.campaign = make_CampaignId("edge-campaign");
  request.component = component;
  request.artifact = artifact;
  request.strategy = StrategyKind::redundant_pair_rolling;   // not claimed by this adapter
  request.authority = make_AuthorityId("edge-authority");
  auto plan = engine.value()->plan(request);
  if (!plan.has_value()) {
    std::cerr << "plan failed: " << plan.error().to_string() << "\n";
    return 1;
  }
  std::cout << "plan executable: " << (plan.value().executable() ? "true" : "false") << "\n";
  for (const auto& rejection : plan.value().rejections) {
    std::cout << "  rejected: " << plan_rejection_name(rejection.code) << " [" << rejection.subject
              << "] " << rejection.detail << "\n";
  }

  request.strategy = StrategyKind::in_place;
  auto accepted = engine.value()->plan(request);
  if (!accepted.has_value()) {
    std::cerr << "plan failed: " << accepted.error().to_string() << "\n";
    return 1;
  }
  std::cout << "in-place plan executable: " << (accepted.value().executable() ? "true" : "false")
            << "\n";
  static_cast<void>(engine.value()->shutdown());
  return plan.value().executable() ? 1 : 0;
}