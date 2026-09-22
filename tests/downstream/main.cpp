// Independent downstream consumer of the installed FabricUpgradeManager
// package. It drives a complete synthetic upgrade through the public headers and
// fails if the governance invariants do not hold.
#include <iostream>
#include <memory>

#include <fum/adapters/synthetic_adapter.hpp>
#include <fum/core/fs.hpp>
#include <fum/engine/engine.hpp>

using namespace fum;

int main() {
  auto state = fs::TempDir::create("fum-downstream");
  if (!state.has_value()) {
    std::cerr << "no temporary directory\n";
    return 1;
  }
  const ComponentId component = make_ComponentId("downstream-component");
  const std::string payload = "downstream-artifact";

  ArtifactDescriptor artifact;
  artifact.set_id(make_ArtifactId("downstream-artifact-1.1.0"));
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

  auto adapter = std::make_shared<SyntheticAdapter>(make_AdapterId("downstream-adapter"));
  AdapterClaims claims;
  claims.strategies = {StrategyKind::restart_based};
  claims.supports_prepare = true;
  claims.supports_rollback = true;
  claims.supports_health_probe = true;
  claims.max_parallel_activations = 1;
  adapter->set_claims(claims);

  auto inventory = std::make_shared<SyntheticInventory>();
  for (int i = 1; i <= 2; ++i) {
    const std::string id = "downstream-node-" + std::to_string(i);
    SyntheticTarget target;
    target.id = make_TargetId(id);
    target.component = component;
    target.version = Version::parse("1.0.0").value();
    target.build = make_BuildId("build-1");
    target.platform = "synthetic/x86_64";
    adapter->add_target(target);
    TargetDescriptor descriptor;
    descriptor.id = target.id;
    descriptor.component = component;
    descriptor.display_name = id;
    descriptor.installed_version = target.version;
    descriptor.installed_build = target.build;
    descriptor.platform = target.platform;
    descriptor.adapter = adapter->id();
    descriptor.adapter_kind = AdapterKind::synthetic;
    descriptor.claims = claims;
    descriptor.authority = make_AuthorityId("downstream-authority");
    inventory->add_target(descriptor);
  }

  auto registry = std::make_shared<SyntheticCompatibilityRegistry>("registry-rev-1");
  CompatibilityMatrixEntry entry;
  entry.component = component;
  entry.from = Version::parse("1.0.0").value();
  entry.to = Version::parse("1.1.0").value();
  entry.compatible = true;
  entry.rollback_supported = true;
  registry->add_entry(entry);

  auto artifacts = std::make_shared<SyntheticArtifactSource>();
  if (!artifacts->publish(artifact, payload).has_value()) {
    std::cerr << "could not publish the payload\n";
    return 1;
  }

  EngineConfig config;
  config.store.directory = state.value().path();
  config.policy.id = make_PolicyId("downstream-policy");
  EngineDependencies dependencies;
  dependencies.inventory = inventory;
  dependencies.compatibility = registry;
  dependencies.artifacts = artifacts;
  dependencies.clock = std::make_shared<SystemClock>();
  dependencies.adapters.add(adapter);

  auto engine = Engine::open(config, dependencies);
  if (!engine.has_value()) {
    std::cerr << "engine did not open: " << engine.error().to_string() << "\n";
    return 1;
  }

  UpgradePlanRequest request;
  request.campaign = make_CampaignId("downstream-campaign");
  request.component = component;
  request.artifact = artifact;
  request.strategy = StrategyKind::restart_based;
  request.authority = make_AuthorityId("downstream-authority");

  auto created = engine.value()->create_campaign(request);
  if (!created.has_value()) {
    std::cerr << "create failed: " << created.error().to_string() << "\n";
    return 1;
  }
  auto report = engine.value()->preflight(request.campaign);
  if (!report.has_value() || !report.value().passed) {
    std::cerr << "preflight failed\n";
    return 1;
  }
  if (!engine.value()->start(request.campaign, StartOptions{}).has_value()) {
    std::cerr << "start failed\n";
    return 1;
  }
  if (!engine.value()->run_until_idle().has_value()) {
    std::cerr << "run failed\n";
    return 1;
  }
  auto status = engine.value()->status(request.campaign);
  if (!status.has_value() || status.value().campaign.state != UpgradeState::completed) {
    std::cerr << "campaign did not complete\n";
    return 1;
  }
  auto records = engine.value()->provenance(request.campaign);
  if (!records.has_value() || records.value().size() != 4) {
    std::cerr << "unexpected provenance record count\n";
    return 1;
  }
  const json::Value stats = engine.value()->stats();
  const json::Value* adapters = stats.find("adapters");
  const std::size_t adapter_count = adapters == nullptr ? 0 : adapters->items().size();
  std::cout << "downstream consumer: " << records.value().size()
            << " provenance records, campaign completed under " << adapter_count
            << " adapter(s)\n";
  static_cast<void>(engine.value()->shutdown());
  return 0;
}
