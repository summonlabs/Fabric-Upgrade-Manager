// Benchmark: completed upgrades per second.
//
// The benchmark measures finished work - campaigns driven to a terminal state
// with verified provenance records - not submission or enqueue latency. Every
// upgrade is journaled durably exactly like a production run.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "fum/adapters/synthetic_adapter.hpp"
#include "fum/core/fs.hpp"
#include "fum/engine/engine.hpp"

namespace {

using namespace fum;

struct Scenario {
  std::size_t campaigns = 20;
  std::size_t targets_per_campaign = 8;
  std::size_t workers = 4;
  bool threaded = false;
};

struct Result {
  std::size_t campaigns_completed = 0;
  std::size_t targets_upgraded = 0;
  std::size_t provenance_records = 0;
  std::size_t decisions = 0;
  double seconds = 0.0;
};

Result run_scenario(const Scenario& scenario) {
  auto state = fs::TempDir::create("fum-bench");
  if (!state.has_value()) {
    std::cerr << "no temporary directory\n";
    std::exit(1);
  }
  const ComponentId component = make_ComponentId("bench-component");
  const std::string payload = "benchmark-payload";

  ArtifactDescriptor artifact;
  artifact.set_id(make_ArtifactId("bench-artifact"));
  artifact.set_component(component);
  artifact.set_version(Version::parse("2.0.0").value());
  artifact.set_build(make_BuildId("build-bench"));
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

  auto adapter = std::make_shared<SyntheticAdapter>(make_AdapterId("bench-adapter"));
  AdapterClaims claims;
  claims.strategies = {StrategyKind::restart_based, StrategyKind::in_place};
  claims.supports_prepare = true;
  claims.supports_rollback = true;
  claims.supports_health_probe = true;
  claims.max_parallel_activations = static_cast<std::uint32_t>(scenario.workers);
  adapter->set_claims(claims);

  auto inventory = std::make_shared<SyntheticInventory>();
  for (std::size_t i = 0; i < scenario.targets_per_campaign; ++i) {
    const std::string id = "bench-target-" + std::to_string(i);
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
    descriptor.authority = make_AuthorityId("bench-authority");
    inventory->add_target(descriptor);
  }

  auto registry = std::make_shared<SyntheticCompatibilityRegistry>("registry-rev-1");
  CompatibilityMatrixEntry entry;
  entry.component = component;
  entry.from = Version::parse("1.0.0").value();
  entry.to = Version::parse("2.0.0").value();
  entry.compatible = true;
  entry.rollback_supported = true;
  registry->add_entry(entry);

  auto artifacts = std::make_shared<SyntheticArtifactSource>();
  if (!artifacts->publish(artifact, payload).has_value()) {
    std::cerr << "could not publish the payload\n";
    std::exit(1);
  }

  EngineConfig config;
  config.store.directory = state.value().path();
  config.policy.id = make_PolicyId("bench-policy");
  config.policy.max_concurrent_activations = static_cast<std::uint32_t>(scenario.workers);
  config.executor = scenario.threaded ? ExecutorKind::thread_pool : ExecutorKind::deterministic_inline;
  config.worker_threads = scenario.workers;
  EngineDependencies dependencies;
  dependencies.inventory = inventory;
  dependencies.compatibility = registry;
  dependencies.artifacts = artifacts;
  dependencies.clock = std::make_shared<SystemClock>();
  dependencies.adapters.add(adapter);

  auto engine = Engine::open(config, dependencies);
  if (!engine.has_value()) {
    std::cerr << "engine did not open: " << engine.error().to_string() << "\n";
    std::exit(1);
  }

  Result result;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < scenario.campaigns; ++i) {
    UpgradePlanRequest request;
    request.campaign = make_CampaignId("bench-campaign-" + std::to_string(i));
    request.component = component;
    request.artifact = artifact;
    request.strategy = StrategyKind::restart_based;
    request.authority = make_AuthorityId("bench-authority");
    request.max_parallel_activations = static_cast<std::uint32_t>(scenario.workers);
    // This scenario crosses a major version, so the budget must say so.
    SkewBudget budget;
    budget.max_major_skew = 1;
    budget.max_minor_skew = 4;
    budget.max_patch_skew = 32;
    request.skew_budget = budget;
    auto created = engine.value()->create_campaign(request);
    if (!created.has_value()) {
      std::cerr << "create failed: " << created.error().to_string() << "\n";
      std::exit(1);
    }
    auto report = engine.value()->preflight(request.campaign);
    if (!report.has_value()) {
      std::cerr << "preflight error: " << report.error().to_string() << "\n";
      std::exit(1);
    }
    if (!report.value().passed) {
      std::cerr << "preflight failed:";
      for (const auto& blocker : report.value().blockers) {
        std::cerr << "\n  " << blocker;
      }
      std::cerr << "\n";
      std::exit(1);
    }
    if (scenario.threaded) {
      auto started = engine.value()->start(request.campaign, StartOptions{});
      if (!started.has_value()) {
        std::cerr << "start failed: " << started.error().to_string() << "\n";
        std::exit(1);
      }
      static_cast<void>(engine.value()->wait_for_idle());
    } else {
      auto started = engine.value()->start(request.campaign, StartOptions{});
      if (!started.has_value()) {
        std::cerr << "start failed: " << started.error().to_string() << "\n";
        std::exit(1);
      }
      static_cast<void>(engine.value()->run_until_idle());
    }
    auto status = engine.value()->status(request.campaign);
    if (!status.has_value()) {
      std::cerr << "status failed\n";
      std::exit(1);
    }
    if (status.value().campaign.state == UpgradeState::completed) {
      ++result.campaigns_completed;
    }
    for (const auto& attempt : status.value().attempts) {
      if (attempt.state == AttemptState::verified) {
        ++result.targets_upgraded;
      }
    }
    result.decisions += status.value().campaign.decisions_recorded;
  }
  const auto finish = std::chrono::steady_clock::now();
  result.seconds = std::chrono::duration<double>(finish - start).count();
  auto ledger_records = engine.value()->provenance(CampaignId{});
  if (ledger_records.has_value()) {
    result.provenance_records = ledger_records.value().size();
  }
  static_cast<void>(engine.value()->shutdown());
  return result;
}

void report(const Scenario& scenario, const Result& result) {
  const double upgrades_per_second =
      result.seconds > 0.0 ? static_cast<double>(result.campaigns_completed) / result.seconds : 0.0;
  const double targets_per_second =
      result.seconds > 0.0 ? static_cast<double>(result.targets_upgraded) / result.seconds : 0.0;
  std::printf(
      "scenario: campaigns=%zu targets=%zu workers=%zu executor=%s\n"
      "  completed campaigns : %zu\n"
      "  verified targets    : %zu\n"
      "  provenance records  : %zu\n"
      "  elapsed             : %.3f s\n"
      "  completed upgrades/s: %.1f\n"
      "  verified targets/s  : %.1f\n\n",
      scenario.campaigns, scenario.targets_per_campaign, scenario.workers,
      scenario.threaded ? "thread-pool" : "deterministic-inline", result.campaigns_completed,
      result.targets_upgraded, result.provenance_records, result.seconds, upgrades_per_second,
      targets_per_second);
}

}  // namespace

int main(int argc, char** argv) {
  Scenario baseline;
  baseline.campaigns = 20;
  baseline.targets_per_campaign = 8;
  baseline.workers = 1;

  Scenario concurrent;
  concurrent.campaigns = 20;
  concurrent.targets_per_campaign = 8;
  concurrent.workers = 4;
  concurrent.threaded = true;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument.rfind("--campaigns=", 0) == 0) {
      baseline.campaigns = static_cast<std::size_t>(std::stoul(argument.substr(12)));
      concurrent.campaigns = baseline.campaigns;
    } else if (argument.rfind("--targets=", 0) == 0) {
      baseline.targets_per_campaign = static_cast<std::size_t>(std::stoul(argument.substr(10)));
      concurrent.targets_per_campaign = baseline.targets_per_campaign;
    } else if (argument.rfind("--workers=", 0) == 0) {
      concurrent.workers = static_cast<std::size_t>(std::stoul(argument.substr(10)));
    }
  }
  std::printf("Fabric Upgrade Manager benchmark: completed work, durable journaling on\n\n");
  report(baseline, run_scenario(baseline));
  report(concurrent, run_scenario(concurrent));
  return 0;
}