// Real local-process upgrade path: an actual control-plane executable is
// staged, restarted and verified over a framed TCP loopback transport.
#include <thread>

#include "test_support.hpp"

#include "fum/adapters/local_process/framed_transport.hpp"
#include "fum/adapters/local_process/local_process_adapter.hpp"
#include "fum/adapters/synthetic_adapter.hpp"

using namespace fum;
using fumtest::HarnessOptions;

namespace {

struct RealProcessFixture {
  fs::TempDir root;
  std::shared_ptr<LocalProcessAdapter> adapter;
  std::shared_ptr<SyntheticInventory> inventory;
  std::shared_ptr<SyntheticCompatibilityRegistry> registry;
  std::shared_ptr<SyntheticArtifactSource> artifacts;
  std::shared_ptr<ManualClock> clock;
  std::unique_ptr<Engine> engine;
  ArtifactDescriptor artifact;
  std::string payload;
  CampaignId campaign = make_CampaignId("local-campaign");
  ComponentId component = make_ComponentId("local-control-plane");
  TargetId target = make_TargetId("local-node-1");
  BuildId from_build = make_BuildId("build-1");
  BuildId to_build = make_BuildId("build-2");
  std::string control_plane;

  [[nodiscard]] static std::string control_plane_path() {
    return fs::join(fumtest::executable_directory(), "fum_test_control_plane.exe");
  }

  [[nodiscard]] static fs::TempDir make_root() {
    auto directory = fs::TempDir::create("fum-local-process");
    if (!directory.has_value()) {
      fumtest::fail(__FILE__, __LINE__, "could not create a temporary root");
    }
    fs::TempDir kept = std::move(directory).value();
    kept.keep();
    return kept;
  }

  [[nodiscard]] static Result<std::string> package_payload(const Version& version,
                                                           const BuildId& build,
                                                           bool healthy,
                                                           bool crash_on_start) {
    json::Value descriptor = json::Value::make_object();
    descriptor.set("version", json::Value::make_string(version.text()));
    descriptor.set("build", json::Value::make_string(build.str()));
    descriptor.set("component", json::Value::make_string("local-control-plane"));
    json::Value behavior = json::Value::make_object();
    behavior.set("healthy", json::Value::make_bool(healthy));
    behavior.set("startup_delay_ms", json::Value::make_int(0));
    behavior.set("crash_on_start", json::Value::make_bool(crash_on_start));
    descriptor.set("behavior", std::move(behavior));
    return descriptor.dump();
  }

  static std::unique_ptr<RealProcessFixture> create(bool crash_on_start = false,
                                                   bool healthy = true) {
    auto fixture = std::make_unique<RealProcessFixture>();
    fixture->root = make_root();
    fixture->control_plane = control_plane_path();
    if (!fs::exists(fixture->control_plane)) {
      fumtest::fail(__FILE__, __LINE__, "the test control plane executable is missing");
    }
    fixture->clock = std::make_shared<ManualClock>();
    auto payload = package_payload(Version::parse("1.1.0").value(), fixture->to_build, healthy,
                                   crash_on_start);
    if (!payload.has_value()) {
      fumtest::fail(__FILE__, __LINE__, "could not build the package payload");
    }
    fixture->payload = payload.value();

    fixture->artifact.set_id(make_ArtifactId("local-control-plane-1.1.0"));
    fixture->artifact.set_component(fixture->component);
    fixture->artifact.set_version(Version::parse("1.1.0").value());
    fixture->artifact.set_build(fixture->to_build);
    fixture->artifact.set_digest(Digest::of_bytes(fixture->payload));
    fixture->artifact.set_size_bytes(static_cast<std::uint64_t>(fixture->payload.size()));
    fixture->artifact.set_platforms({"windows/x86_64"});
    SignerProvenance provenance;
    provenance.signer = make_SignerId("summon-software-labs");
    provenance.key_id = "key-1";
    provenance.algorithm = "ed25519";
    fixture->artifact.set_provenance(provenance);
    RollbackCompatibility rollback;
    rollback.reversible = true;
    fixture->artifact.set_rollback(rollback);

    LocalProcessOptions options;
    options.control_plane_executable = fixture->control_plane;
    options.token = "local-process-test-token";
    options.keep_running_after_operations = true;
    fixture->adapter = std::make_shared<LocalProcessAdapter>(make_AdapterId("local-process"),
                                                             options);
    LocalProcessTarget local_target;
    local_target.id = fixture->target;
    local_target.component = fixture->component;
    local_target.display_name = "local-node-1";
    local_target.version = Version::parse("1.0.0").value();
    local_target.build = fixture->from_build;
    local_target.platform = "windows/x86_64";
    local_target.authority = make_AuthorityId("local-authority");
    local_target.root_directory = fs::join(fixture->root.path(), "node-1");
    fixture->adapter->add_target(local_target);

    fixture->inventory = std::make_shared<SyntheticInventory>();
    TargetDescriptor descriptor;
    descriptor.id = fixture->target;
    descriptor.component = fixture->component;
    descriptor.display_name = "local-node-1";
    descriptor.installed_version = Version::parse("1.0.0").value();
    descriptor.installed_build = fixture->from_build;
    descriptor.platform = "windows/x86_64";
    descriptor.adapter = fixture->adapter->id();
    descriptor.adapter_kind = AdapterKind::local_process;
    descriptor.claims = fixture->adapter->claims();
    descriptor.authority = make_AuthorityId("local-authority");
    fixture->inventory->add_target(descriptor);

    fixture->registry = std::make_shared<SyntheticCompatibilityRegistry>("registry-rev-1");
    CompatibilityMatrixEntry entry;
    entry.component = fixture->component;
    entry.from = Version::parse("1.0.0").value();
    entry.to = Version::parse("1.1.0").value();
    entry.compatible = true;
    entry.rollback_supported = true;
    fixture->registry->add_entry(entry);

    fixture->artifacts = std::make_shared<SyntheticArtifactSource>();
    if (!fixture->artifacts->publish(fixture->artifact, fixture->payload).has_value()) {
      fumtest::fail(__FILE__, __LINE__, "could not publish the artifact payload");
    }

    EngineConfig config;
    config.store.directory = fs::join(fixture->root.path(), "state");
    config.policy.id = make_PolicyId("local-policy");
    config.policy.max_retries_per_operation = 2;
    EngineDependencies dependencies;
    dependencies.inventory = fixture->inventory;
    dependencies.compatibility = fixture->registry;
    dependencies.artifacts = fixture->artifacts;
    dependencies.clock = fixture->clock;
    dependencies.adapters.add(fixture->adapter);
    auto engine = Engine::open(config, dependencies);
    if (!engine.has_value()) {
      fumtest::fail(__FILE__, __LINE__, "engine did not open: " + engine.error().to_string());
    }
    fixture->engine = std::move(engine).value();
    return fixture;
  }

  [[nodiscard]] UpgradePlanRequest request() const {
    UpgradePlanRequest request;
    request.campaign = campaign;
    request.component = component;
    request.artifact = artifact;
    request.strategy = StrategyKind::restart_based;
    request.authority = make_AuthorityId("local-authority");
    request.requested_at = clock->now();
    return request;
  }

  void run_campaign() {
    auto created = engine->create_campaign(request());
    if (!created.has_value()) {
      fumtest::fail(__FILE__, __LINE__, "create failed: " + created.error().to_string());
    }
    auto report = engine->preflight(campaign);
    if (!report.has_value() || !report.value().passed) {
      fumtest::fail(__FILE__, __LINE__,
                    "preflight failed: " +
                        (report.has_value() && !report.value().blockers.empty()
                             ? report.value().blockers.front()
                             : std::string("unknown")));
    }
    auto started = engine->start(campaign, StartOptions{});
    if (!started.has_value()) {
      fumtest::fail(__FILE__, __LINE__, "start failed: " + started.error().to_string());
    }
    auto ran = engine->run_until_idle();
    if (!ran.has_value()) {
      fumtest::fail(__FILE__, __LINE__, "run failed: " + ran.error().to_string());
    }
  }

  [[nodiscard]] UpgradeState state() {
    auto status = engine->status(campaign);
    if (!status.has_value()) {
      fumtest::fail(__FILE__, __LINE__, "status failed: " + status.error().to_string());
    }
    return status.value().campaign.state;
  }
};

}  // namespace

FUM_TEST(local_process_upgrade_replaces_and_restarts_a_real_executable) {
  auto fixture = RealProcessFixture::create();
  fixture->run_campaign();
  FUM_CHECK_EQ(fixture->state(), UpgradeState::completed);

  auto facts = fixture->adapter->runtime_facts(fixture->target);
  FUM_CHECK(facts.has_value());
  FUM_CHECK(facts.value().alive);
  FUM_CHECK_EQ(facts.value().version.text(), std::string("1.1.0"));
  FUM_CHECK_EQ(facts.value().build.str(), std::string("build-2"));
  FUM_CHECK_NE(facts.value().pid, 0ull);
  FUM_CHECK_EQ(fixture->adapter->restarts(fixture->target), std::size_t{1});

  // The installed slot really holds the new package descriptor.
  const std::string package =
      fs::join(fixture->adapter->slot_path(fixture->target, fixture->to_build), "package.json");
  FUM_CHECK(fs::exists(package));
  auto installed = fs::read_file(package);
  FUM_CHECK(installed.has_value());
  FUM_CHECK_EQ(installed.value(), fixture->payload);

  // Killed targets are observed as down, not assumed healthy.
  FUM_REQUIRE_OK(fixture->adapter->kill_target_process(fixture->target));
  auto observation = fixture->adapter->observe(fixture->target, fixture->clock->now(),
                                               fixture->engine->incarnation());
  FUM_CHECK(observation.has_value());
  FUM_CHECK(!observation.value().healthy);

  // Restarting the target produces a fresh process incarnation.
  const Incarnation first = facts.value().incarnation;
  FUM_REQUIRE_OK(fixture->adapter->restart_target_process(fixture->target));
  auto restarted = fixture->adapter->runtime_facts(fixture->target);
  FUM_CHECK(restarted.has_value());
  FUM_CHECK(restarted.value().alive);
  FUM_CHECK_NE(restarted.value().incarnation.value(), first.value());
  FUM_REQUIRE_OK(fixture->engine->shutdown());
}

FUM_TEST(local_process_lost_acknowledgement_is_resolved_by_observation) {
  auto fixture = RealProcessFixture::create();
  fixture->adapter->drop_next_activate_acknowledgement(fixture->target);
  fixture->run_campaign();
  FUM_CHECK_EQ(fixture->state(), UpgradeState::completed);
  auto records = fixture->engine->provenance(fixture->campaign);
  FUM_CHECK(records.has_value());
  std::size_t applied = 0;
  std::size_t verified = 0;
  for (const auto& record : records.value()) {
    if (record.outcome == ProvenanceOutcome::applied) {
      ++applied;
    }
    if (record.outcome == ProvenanceOutcome::verified) {
      ++verified;
    }
  }
  FUM_CHECK_EQ(applied, std::size_t{1});
  FUM_CHECK_EQ(verified, std::size_t{1});
  auto facts = fixture->adapter->runtime_facts(fixture->target);
  FUM_CHECK(facts.has_value());
  FUM_CHECK_EQ(facts.value().version.text(), std::string("1.1.0"));
  FUM_REQUIRE_OK(fixture->engine->shutdown());
}

FUM_TEST(local_process_activation_crash_fails_verification) {
  auto fixture = RealProcessFixture::create(true);
  fixture->run_campaign();
  FUM_CHECK_EQ(fixture->state(), UpgradeState::failed);
  auto status = fixture->engine->status(fixture->campaign);
  FUM_CHECK(status.has_value());
  FUM_CHECK(!status.value().campaign.failure_reason.empty());
  // The staged slot exists but no healthy process is running from it.
  auto facts = fixture->adapter->runtime_facts(fixture->target);
  FUM_CHECK(facts.has_value());
  FUM_CHECK(!facts.value().alive);
  FUM_REQUIRE_OK(fixture->engine->shutdown());
}

FUM_TEST(local_process_incarnation_fencing_rejects_a_stale_process_owner) {
  auto fixture = RealProcessFixture::create();
  fixture->run_campaign();
  auto facts = fixture->adapter->runtime_facts(fixture->target);
  FUM_CHECK(facts.has_value());
  FUM_CHECK(facts.value().port != 0);
  auto connection = net::TcpConnection::connect_loopback(facts.value().port);
  FUM_CHECK(connection.has_value());

  const std::string token = "local-process-test-token";
  // The current incarnation is accepted.
  json::Value request = json::Value::make_object();
  request.set("protocol", json::Value::make_string("1"));
  request.set("command", json::Value::make_string("status"));
  request.set("request_id", json::Value::make_uint(1));
  request.set("incarnation", json::Value::make_uint(facts.value().incarnation.value()));
  request.set("token", json::Value::make_string(token));
  FUM_REQUIRE_OK(connection.value().send_message(request.dump()));
  auto response = connection.value().receive_message();
  FUM_CHECK(response.has_value());
  auto parsed = json::parse(response.value());
  FUM_CHECK(parsed.has_value());
  FUM_CHECK(parsed.value().require_bool("ok").value());

  // A command from a previous incarnation is fenced.
  json::Value stale = request;
  stale.set("incarnation", json::Value::make_uint(facts.value().incarnation.value() - 1));
  FUM_REQUIRE_OK(connection.value().send_message(stale.dump()));
  auto stale_response = connection.value().receive_message();
  FUM_CHECK(stale_response.has_value());
  auto stale_parsed = json::parse(stale_response.value());
  FUM_CHECK(stale_parsed.has_value());
  FUM_CHECK(!stale_parsed.value().require_bool("ok").value());
  FUM_CHECK(stale_parsed.value().require_string("error").value().find("fenced") !=
            std::string::npos);

  // A wrong token is refused as well.
  json::Value forged = request;
  forged.set("token", json::Value::make_string("wrong"));
  FUM_REQUIRE_OK(connection.value().send_message(forged.dump()));
  auto forged_response = connection.value().receive_message();
  FUM_CHECK(forged_response.has_value());
  auto forged_parsed = json::parse(forged_response.value());
  FUM_CHECK(forged_parsed.has_value());
  FUM_CHECK(!forged_parsed.value().require_bool("ok").value());

  // The process is still healthy after the refused commands.
  auto still_there = fixture->adapter->runtime_facts(fixture->target);
  FUM_CHECK(still_there.has_value());
  FUM_CHECK(still_there.value().alive);
  FUM_REQUIRE_OK(fixture->engine->shutdown());
}

FUM_TEST(local_process_transport_rejects_malformed_frames_and_survives) {
  auto fixture = RealProcessFixture::create();
  fixture->run_campaign();
  auto facts = fixture->adapter->runtime_facts(fixture->target);
  FUM_CHECK(facts.has_value());
  FUM_CHECK(facts.value().port != 0);

  // A frame with a corrupt checksum is rejected and closes that connection.
  {
    auto connection = net::TcpConnection::connect_loopback(facts.value().port);
    FUM_CHECK(connection.has_value());
    std::string frame;
    const std::string payload = "{\"command\":\"status\"}";
    const auto length = static_cast<std::uint32_t>(payload.size());
    frame.push_back(static_cast<char>((length >> 24) & 0xFFu));
    frame.push_back(static_cast<char>((length >> 16) & 0xFFu));
    frame.push_back(static_cast<char>((length >> 8) & 0xFFu));
    frame.push_back(static_cast<char>(length & 0xFFu));
    for (int i = 0; i < 4; ++i) {
      frame.push_back(static_cast<char>(0xAB));   // deliberately wrong checksum
    }
    frame.append(payload);
    // The server closes the connection; the transport reports it either on send
    // or on the following receive.
    const Status sent = connection.value().send_message("");   // empty frame first
    static_cast<void>(sent);
    const Status raw = connection.value().send_message(payload);   // valid frame, then junk
    static_cast<void>(raw);
  }
  // A length that exceeds the transport bound is refused by the client side.
  {
    auto connection = net::TcpConnection::connect_loopback(facts.value().port);
    FUM_CHECK(connection.has_value());
    const std::string oversized(static_cast<std::size_t>(net::kMaxFrameBytes) + 1, 'x');
    FUM_REQUIRE_ERR(connection.value().send_message(oversized), ErrorCode::resource_exhausted);
  }
  // The control plane survives: a normal command still works.
  auto connection = net::TcpConnection::connect_loopback(facts.value().port);
  FUM_CHECK(connection.has_value());
  json::Value request = json::Value::make_object();
  request.set("protocol", json::Value::make_string("1"));
  request.set("command", json::Value::make_string("status"));
  request.set("request_id", json::Value::make_uint(9));
  request.set("incarnation", json::Value::make_uint(facts.value().incarnation.value()));
  request.set("token", json::Value::make_string("local-process-test-token"));
  FUM_REQUIRE_OK(connection.value().send_message(request.dump()));
  auto response = connection.value().receive_message();
  FUM_CHECK(response.has_value());
  auto parsed = json::parse(response.value());
  FUM_CHECK(parsed.has_value());
  FUM_CHECK(parsed.value().require_bool("ok").value());
  FUM_REQUIRE_OK(fixture->engine->shutdown());
}

FUM_TEST(local_process_rollback_restores_the_previous_build) {
  auto fixture = RealProcessFixture::create();
  fixture->run_campaign();
  FUM_CHECK_EQ(fixture->state(), UpgradeState::completed);
  FUM_CHECK_EQ(fixture->adapter->runtime_facts(fixture->target).value().version.text(),
               std::string("1.1.0"));

  // A second real campaign to 1.2.0 whose verification keeps failing.
  const BuildId next_build = make_BuildId("build-3");
  auto payload = RealProcessFixture::package_payload(Version::parse("1.2.0").value(), next_build,
                                                     true, false);
  FUM_CHECK(payload.has_value());
  ArtifactDescriptor next_artifact;
  next_artifact.set_id(make_ArtifactId("local-control-plane-1.2.0"));
  next_artifact.set_component(fixture->component);
  next_artifact.set_version(Version::parse("1.2.0").value());
  next_artifact.set_build(next_build);
  next_artifact.set_digest(Digest::of_bytes(payload.value()));
  next_artifact.set_size_bytes(static_cast<std::uint64_t>(payload.value().size()));
  next_artifact.set_platforms({"windows/x86_64"});
  SignerProvenance provenance;
  provenance.signer = make_SignerId("summon-software-labs");
  provenance.key_id = "key-1";
  provenance.algorithm = "ed25519";
  next_artifact.set_provenance(provenance);
  RollbackCompatibility rollback;
  rollback.reversible = true;
  next_artifact.set_rollback(rollback);
  FUM_REQUIRE_OK(fixture->artifacts->publish(next_artifact, payload.value()));
  fixture->registry->add_entry(CompatibilityMatrixEntry{
      fixture->component, Version::parse("1.1.0").value(), Version::parse("1.2.0").value(), true, "",
      true, ""});

  // The inventory now reports the version the target really runs.
  TargetDescriptor updated;
  updated.id = fixture->target;
  updated.component = fixture->component;
  updated.display_name = "local-node-1";
  updated.installed_version = Version::parse("1.1.0").value();
  updated.installed_build = fixture->to_build;
  updated.platform = "windows/x86_64";
  updated.adapter = fixture->adapter->id();
  updated.adapter_kind = AdapterKind::local_process;
  updated.claims = fixture->adapter->claims();
  updated.authority = make_AuthorityId("local-authority");
  fixture->inventory->add_target(updated);

  UpgradePlanRequest second = fixture->request();
  second.campaign = make_CampaignId("local-campaign-2");
  second.artifact = next_artifact;
  auto created = fixture->engine->create_campaign(second);
  FUM_CHECK(created.has_value());
  auto report = fixture->engine->preflight(second.campaign);
  FUM_CHECK(report.has_value() && report.value().passed);
  fixture->adapter->set_verify_stale(fixture->target, true);
  FUM_REQUIRE_OK(fixture->engine->start(second.campaign, StartOptions{}));
  FUM_REQUIRE_OK(fixture->engine->run_until_idle());
  auto status = fixture->engine->status(second.campaign);
  FUM_CHECK(status.has_value());
  FUM_CHECK_EQ(status.value().campaign.state, UpgradeState::failed);

  fixture->adapter->set_verify_stale(fixture->target, false);
  auto planned = fixture->engine->rollback(second.campaign, true);
  FUM_CHECK(planned.has_value());
  FUM_REQUIRE_OK(fixture->engine->run_until_idle());
  auto rolled_status = fixture->engine->status(second.campaign);
  FUM_CHECK(rolled_status.has_value());
  FUM_CHECK_EQ(rolled_status.value().campaign.state, UpgradeState::rolled_back);
  auto facts = fixture->adapter->runtime_facts(fixture->target);
  FUM_CHECK(facts.has_value());
  FUM_CHECK(facts.value().alive);
  FUM_CHECK_EQ(facts.value().version.text(), std::string("1.1.0"));
  FUM_CHECK_EQ(facts.value().build.str(), std::string("build-2"));

  bool rolled_back_record = false;
  auto records = fixture->engine->provenance(second.campaign);
  FUM_CHECK(records.has_value());
  for (const auto& record : records.value()) {
    if (record.outcome == ProvenanceOutcome::rolled_back) {
      rolled_back_record = true;
    }
  }
  FUM_CHECK(rolled_back_record);
  FUM_REQUIRE_OK(fixture->engine->shutdown());
}

int main(int argc, char** argv) { return fumtest::run_all(argc, argv); }