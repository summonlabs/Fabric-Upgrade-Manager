// Example: a deterministic rolling upgrade of a synthetic three-target group,
// including the plan, the preflight decision trail and the provenance ledger.
//
// This example uses SYNTHETIC adapters: no vendor hardware is involved and none
// is claimed. See examples/custom_adapter.cpp for the adapter interface and
// tests/test_local_process.cpp for the real local-process path.
#include <iostream>
#include <memory>

#include "fum/adapters/synthetic_adapter.hpp"
#include "fum/core/fs.hpp"
#include "fum/engine/engine.hpp"

namespace {

using namespace fum;

struct Demo {
  std::shared_ptr<ManualClock> clock;
  std::shared_ptr<SyntheticAdapter> adapter;
  std::shared_ptr<SyntheticInventory> inventory;
  std::shared_ptr<SyntheticCompatibilityRegistry> registry;
  std::shared_ptr<SyntheticArtifactSource> artifacts;
  std::shared_ptr<SyntheticDrainFabric> drain;
  std::shared_ptr<Engine> engine;
  ArtifactDescriptor artifact;
  CampaignId campaign = make_CampaignId("demo-campaign");
  ComponentId component = make_ComponentId("fabric-core");
  std::string payload = "demo-artifact-payload";
  fs::TempDir state;
};

bool build_demo(Demo& demo) {
  auto state = fs::TempDir::create("fum-example");
  if (!state.has_value()) {
    std::cerr << "could not create a temporary state directory\n";
    return false;
  }
  demo.state = std::move(state).value();
  demo.clock = std::make_shared<ManualClock>();

  demo.artifact.set_id(make_ArtifactId("fabric-core-1.1.0"));
  demo.artifact.set_component(demo.component);
  demo.artifact.set_version(Version::parse("1.1.0").value());
  demo.artifact.set_build(make_BuildId("build-2"));
  demo.artifact.set_digest(Digest::of_bytes(demo.payload));
  demo.artifact.set_size_bytes(static_cast<std::uint64_t>(demo.payload.size()));
  demo.artifact.set_platforms({"synthetic/x86_64"});
  SignerProvenance provenance;
  provenance.signer = make_SignerId("summon-software-labs");
  provenance.key_id = "key-1";
  provenance.algorithm = "ed25519";
  demo.artifact.set_provenance(provenance);
  RollbackCompatibility rollback;
  rollback.reversible = true;
  demo.artifact.set_rollback(rollback);

  demo.adapter = std::make_shared<SyntheticAdapter>(make_AdapterId("synthetic-adapter"));
  AdapterClaims claims;
  claims.strategies = {StrategyKind::in_place, StrategyKind::restart_based,
                       StrategyKind::redundant_pair_rolling};
  claims.supports_prepare = true;
  claims.supports_rollback = true;
  claims.supports_health_probe = true;
  claims.max_parallel_activations = 1;
  demo.adapter->set_claims(claims);

  demo.inventory = std::make_shared<SyntheticInventory>();
  for (int i = 1; i <= 3; ++i) {
    const std::string id = "fabric-node-" + std::to_string(i);
    SyntheticTarget target;
    target.id = make_TargetId(id);
    target.component = demo.component;
    target.display_name = id;
    target.version = Version::parse("1.0.0").value();
    target.build = make_BuildId("build-1");
    target.platform = "synthetic/x86_64";
    target.capabilities = {"hot-restart"};
    target.redundancy_group = "pair-a";
    demo.adapter->add_target(target);

    TargetDescriptor descriptor;
    descriptor.id = target.id;
    descriptor.component = demo.component;
    descriptor.display_name = id;
    descriptor.installed_version = target.version;
    descriptor.installed_build = target.build;
    descriptor.platform = target.platform;
    descriptor.capabilities = target.capabilities;
    descriptor.adapter = demo.adapter->id();
    descriptor.adapter_kind = AdapterKind::synthetic;
    descriptor.claims = claims;
    descriptor.authority = make_AuthorityId("fabric-authority");
    descriptor.redundancy_group = "pair-a";
    demo.inventory->add_target(descriptor);
  }

  demo.registry = std::make_shared<SyntheticCompatibilityRegistry>("registry-rev-1");
  CompatibilityMatrixEntry entry;
  entry.component = demo.component;
  entry.from = Version::parse("1.0.0").value();
  entry.to = Version::parse("1.1.0").value();
  entry.compatible = true;
  entry.rollback_supported = true;
  demo.registry->add_entry(entry);

  demo.artifacts = std::make_shared<SyntheticArtifactSource>();
  if (!demo.artifacts->publish(demo.artifact, demo.payload).has_value()) {
    std::cerr << "could not publish the artifact payload\n";
    return false;
  }
  demo.drain = std::make_shared<SyntheticDrainFabric>();

  EngineConfig config;
  config.store.directory = demo.state.path();
  config.policy.id = make_PolicyId("demo-policy");
  config.policy.revision = Revision(1);
  EngineDependencies dependencies;
  dependencies.inventory = demo.inventory;
  dependencies.compatibility = demo.registry;
  dependencies.artifacts = demo.artifacts;
  dependencies.drain = demo.drain;
  dependencies.clock = demo.clock;
  dependencies.adapters.add(demo.adapter);
  auto engine = Engine::open(config, dependencies);
  if (!engine.has_value()) {
    std::cerr << "engine did not open: " << engine.error().to_string() << "\n";
    return false;
  }
  demo.engine = std::move(engine).value();
  return true;
}

void print_plan(const UpgradePlan& plan) {
  std::cout << "plan " << plan.fingerprint().substr(0, 16) << " strategy "
            << strategy_kind_name(plan.strategy) << " stages " << plan.stages.size() << "\n";
  for (const auto& stage : plan.stages) {
    std::cout << "  " << stage.name << ":";
    for (const auto& target : stage.targets) {
      std::cout << " " << target.str();
    }
    std::cout << "\n";
  }
}

}  // namespace

int main() {
  Demo demo;
  if (!build_demo(demo)) {
    return 1;
  }
  UpgradePlanRequest request;
  request.campaign = demo.campaign;
  request.component = demo.component;
  request.artifact = demo.artifact;
  request.strategy = StrategyKind::restart_based;
  request.authority = make_AuthorityId("fabric-authority");
  request.description = "example rolling upgrade";
  request.max_parallel_activations = 1;
  request.requested_at = demo.clock->now();

  auto plan = demo.engine->plan(request);
  if (!plan.has_value()) {
    std::cerr << "plan failed: " << plan.error().to_string() << "\n";
    return 1;
  }
  print_plan(plan.value());

  auto created = demo.engine->create_campaign(request);
  if (!created.has_value()) {
    std::cerr << "create failed: " << created.error().to_string() << "\n";
    return 1;
  }
  auto report = demo.engine->preflight(demo.campaign);
  if (!report.has_value()) {
    std::cerr << "preflight failed: " << report.error().to_string() << "\n";
    return 1;
  }
  std::cout << "preflight passed=" << (report.value().passed ? "true" : "false")
            << " fingerprint=" << report.value().fingerprint.substr(0, 16) << "\n";
  for (const auto& blocker : report.value().blockers) {
    std::cout << "  blocker: " << blocker << "\n";
  }
  auto started = demo.engine->start(demo.campaign, StartOptions{});
  if (!started.has_value()) {
    std::cerr << "start refused: " << started.error().to_string() << "\n";
    return 1;
  }
  auto ran = demo.engine->run_until_idle();
  if (!ran.has_value()) {
    std::cerr << "run failed: " << ran.error().to_string() << "\n";
    return 1;
  }
  auto status = demo.engine->status(demo.campaign);
  if (!status.has_value()) {
    std::cerr << "status failed: " << status.error().to_string() << "\n";
    return 1;
  }
  std::cout << "final state: " << upgrade_state_name(status.value().campaign.state) << "\n";
  for (const auto& attempt : status.value().attempts) {
    std::cout << "  attempt " << attempt.target.str() << " " << attempt_state_name(attempt.state)
              << " observed " << attempt.observed_version.text() << "\n";
  }
  auto provenance = demo.engine->provenance(demo.campaign);
  if (!provenance.has_value()) {
    std::cerr << "provenance failed: " << provenance.error().to_string() << "\n";
    return 1;
  }
  std::cout << "provenance records: " << provenance.value().size() << "\n";
  for (const auto& record : provenance.value()) {
    std::cout << "  " << provenance_outcome_name(record.outcome) << " " << record.target.str()
              << " " << record.from_version.text() << " -> " << record.to_version.text()
              << " chain " << record.chain.hex().substr(0, 12) << "\n";
  }
  static_cast<void>(demo.engine->shutdown());
  return status.value().campaign.state == UpgradeState::completed ? 0 : 1;
}
