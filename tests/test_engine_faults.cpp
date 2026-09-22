#include <thread>

#include "test_support.hpp"

#include "fum/store/state.hpp"

using namespace fum;
using fumtest::Harness;
using fumtest::HarnessOptions;

namespace {

std::size_t count_outcome(Harness& harness, ProvenanceOutcome outcome) {
  auto records = harness.engine().provenance(harness.campaign());
  if (!records.has_value()) {
    return 0;
  }
  std::size_t count = 0;
  for (const auto& record : records.value()) {
    if (record.outcome == outcome) {
      ++count;
    }
  }
  return count;
}

bool has_fence_decision(Harness& harness) {
  auto decisions = harness.engine().decisions(harness.campaign());
  if (!decisions.has_value()) {
    return false;
  }
  for (const auto& decision : decisions.value()) {
    if (decision.kind == DecisionKind::fence) {
      return true;
    }
  }
  return false;
}

}  // namespace

FUM_TEST(lost_acknowledgement_is_resolved_by_observation) {
  HarnessOptions options;
  options.target_count = 1;
  auto harness = Harness::create(options);
  harness->adapter().enqueue_fault(harness->target(0), SyntheticFault::activate_timeout_effect);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
  // The activation was applied once and proven by observation, not repeated.
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, 1ull);
  FUM_CHECK_EQ(count_outcome(*harness, ProvenanceOutcome::applied), std::size_t{1});
  FUM_CHECK_EQ(count_outcome(*harness, ProvenanceOutcome::verified), std::size_t{1});
  FUM_CHECK_EQ(harness->adapter().counters().duplicate_operations, 0ull);
}

FUM_TEST(lost_acknowledgement_without_effect_is_retried_once) {
  HarnessOptions options;
  options.target_count = 1;
  auto harness = Harness::create(options);
  harness->adapter().enqueue_fault(harness->target(0), SyntheticFault::activate_timeout_noop);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
  // The first attempt was lost and retried; the artifact was applied exactly once.
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, 2ull);
  FUM_CHECK_EQ(count_outcome(*harness, ProvenanceOutcome::applied), std::size_t{1});
  FUM_CHECK_EQ(harness->adapter().version_of(harness->target(0)).text(), std::string("1.1.0"));
}

FUM_TEST(activation_crash_is_detected_by_verification) {
  HarnessOptions options;
  options.target_count = 1;
  auto harness = Harness::create(options);
  harness->adapter().enqueue_fault(harness->target(0), SyntheticFault::crash_target);
  run_to_completion(*harness);
  // The version changed but the target is dead: the campaign must not complete.
  FUM_CHECK_EQ(harness->state(), UpgradeState::failed);
  bool found = false;
  for (const auto& attempt : harness->attempts()) {
    if (attempt.state == AttemptState::failed &&
        attempt.last_error.find("unhealthy") != std::string::npos) {
      found = true;
    }
  }
  FUM_CHECK(found);
  FUM_CHECK_EQ(count_outcome(*harness, ProvenanceOutcome::verified), std::size_t{0});
}

FUM_TEST(installer_success_without_a_version_change_is_refused) {
  HarnessOptions options;
  options.target_count = 1;
  auto harness = Harness::create(options);
  harness->adapter().set_persistent_fault(harness->target(0),
                                          SyntheticFault::activate_wrong_version);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::failed);
  auto status = harness->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK(status.value().campaign.failure_reason.find("observed version") != std::string::npos);
  FUM_CHECK_EQ(count_outcome(*harness, ProvenanceOutcome::verified), std::size_t{0});
}

FUM_TEST(stale_verification_evidence_is_retried_and_fails_when_it_stays_stale) {
  HarnessOptions options;
  options.target_count = 1;
  auto harness = Harness::create(options);
  harness->adapter().set_persistent_fault(harness->target(0), SyntheticFault::verify_stale);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::failed);
  auto status = harness->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK(status.value().campaign.failure_reason.find("not fresh") != std::string::npos);
  // Verification was re-attempted up to the retry bound.
  FUM_CHECK_EQ(harness->adapter().counters().verify_calls, 3ull);

  HarnessOptions once;
  once.target_count = 1;
  auto second = Harness::create(once);
  second->adapter().enqueue_fault(second->target(0), SyntheticFault::verify_stale);
  run_to_completion(*second);
  FUM_CHECK_EQ(second->state(), UpgradeState::completed);
  FUM_CHECK_EQ(second->adapter().counters().verify_calls, 2ull);
  const auto attempts = second->attempts();
  FUM_CHECK_EQ(attempts.size(), std::size_t{1});
  FUM_CHECK_EQ(attempts.front().retries, 1u);
}

FUM_TEST(evidence_from_a_previous_incarnation_is_stale) {
  HarnessOptions options;
  options.target_count = 1;
  auto harness = Harness::create(options);
  harness->adapter().set_persistent_fault(harness->target(0),
                                          SyntheticFault::verify_foreign_incarnation);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::failed);
  auto status = harness->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK(status.value().campaign.failure_reason.find("incarnation") != std::string::npos);
}

FUM_TEST(prepare_failure_stops_before_activation) {
  HarnessOptions options;
  options.target_count = 2;
  auto harness = Harness::create(options);
  harness->adapter().set_persistent_fault(harness->target(0), SyntheticFault::prepare_failure);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::failed);
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, 0ull);
  FUM_CHECK_EQ(harness->provenance_count(), std::size_t{0});
}

FUM_TEST(controller_restart_abandons_in_flight_work_and_blocks_the_campaign) {
  HarnessOptions options;
  options.target_count = 2;
  options.strategy = StrategyKind::restart_based;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  FUM_REQUIRE_OK(harness->start(StartOptions{}));
  FUM_REQUIRE_OK(harness->engine().run_steps(2));
  // The campaign is mid-flight: a stage is prepared, activating or verifying.
  const UpgradeState before = harness->state();
  FUM_CHECK(before == UpgradeState::staged || before == UpgradeState::activating ||
            before == UpgradeState::verifying);

  // Simulate a controller crash by opening a second engine over the same state.
  FUM_REQUIRE_OK(harness->reopen());
  FUM_CHECK_EQ(harness->engine().incarnation().value(), 2ull);
  const ReconcileReport reconciliation = harness->engine().reconciliation();
  FUM_CHECK(reconciliation.blocked_campaigns >= 1);
  FUM_CHECK(reconciliation.invalidated_tickets >= 1);
  auto status = harness->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK_EQ(status.value().campaign.state, UpgradeState::blocked);
  FUM_CHECK(status.value().campaign.ticket.issued_at.unix_nanos() == 0);
  FUM_CHECK(!status.value().ticket_current);

  // Nothing further executes while the campaign is blocked.
  const auto counters_before = harness->adapter().counters();
  FUM_REQUIRE_OK(harness->run());
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, counters_before.activate_calls);
  FUM_CHECK_EQ(count_outcome(*harness, ProvenanceOutcome::verified), std::size_t{0});

  // A fresh preflight plus resume continues execution under the new incarnation.
  auto refreshed = harness->engine().preflight(harness->campaign());
  FUM_CHECK(refreshed.has_value() && refreshed.value().passed);
  FUM_REQUIRE_OK(harness->engine().resume(harness->campaign()));
  FUM_REQUIRE_OK(harness->run());
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
  FUM_CHECK_EQ(count_outcome(*harness, ProvenanceOutcome::verified), std::size_t{2});
  for (const auto& attempt : harness->attempts()) {
    if (attempt.state == AttemptState::verified) {
      FUM_CHECK_EQ(attempt.owner.value(), 2ull);
    }
  }
}

FUM_TEST(restart_with_a_persisted_in_flight_attempt_reconciles_conservatively) {
  // Seed durable state exactly as a controller that died mid-activation would
  // leave it, then open a fresh incarnation over that directory.
  auto directory = fs::TempDir::create("fum-restart");
  FUM_CHECK(directory.has_value());
  fs::TempDir guard = std::move(directory).value();
  guard.keep();
  const CampaignId campaign_id = make_CampaignId("crashed-campaign");
  const AttemptId attempt_id = make_AttemptId("crashed-attempt");
  const Timestamp now = Timestamp::from_unix_nanos(1'700'000'000LL * kNanosPerSecond);
  {
    StoreOptions store_options;
    store_options.directory = guard.path();
    auto store = DurableStore::open(store_options);
    FUM_CHECK(store.has_value());
    FUM_CHECK_EQ(store.value().incarnation().value(), 1ull);
    CampaignRecord campaign;
    campaign.id = campaign_id;
    campaign.generation = Generation(2);
    campaign.epoch = Epoch(1);
    campaign.authority = make_AuthorityId("fabric-authority");
    campaign.component = make_ComponentId("fabric-core");
    campaign.state = UpgradeState::activating;
    campaign.strategy = StrategyKind::restart_based;
    campaign.revision = Revision(4);
    campaign.owner_incarnation = Incarnation(1);
    campaign.created_at = now;
    campaign.updated_at = now;
    FUM_REQUIRE_OK(store.value().put_campaign(campaign, now));

    AttemptRecord attempt;
    attempt.id = attempt_id;
    attempt.campaign = campaign_id;
    attempt.generation = Generation(2);
    attempt.stage = make_StageId("stage-1");
    attempt.target = make_TargetId("target-1");
    attempt.artifact = make_ArtifactId("artifact-1");
    attempt.build = make_BuildId("build-2");
    attempt.from_version = Version::parse("1.0.0").value();
    attempt.to_version = Version::parse("1.1.0").value();
    attempt.state = AttemptState::activating;
    attempt.owner = Incarnation(1);
    attempt.authority = make_AuthorityId("fabric-authority");
    attempt.revision = Revision(2);
    attempt.operation_index = Sequence(1);
    attempt.started_at = now;
    attempt.updated_at = now;
    FUM_REQUIRE_OK(store.value().put_attempt(attempt, now));
    FUM_REQUIRE_OK(store.value().close());
  }

  HarnessOptions options;
  options.state_directory = guard.path();
  options.target_count = 1;
  auto harness = Harness::create(options);
  FUM_CHECK_EQ(harness->engine().incarnation().value(), 2ull);
  const ReconcileReport reconciliation = harness->engine().reconciliation();
  FUM_CHECK_EQ(reconciliation.abandoned_attempts, std::size_t{1});
  FUM_CHECK_EQ(reconciliation.blocked_campaigns, std::size_t{1});

  auto status = harness->engine().status(campaign_id);
  FUM_CHECK(status.has_value());
  FUM_CHECK_EQ(status.value().campaign.state, UpgradeState::blocked);
  FUM_CHECK(status.value().campaign.block_reason.find("incarnation 1") != std::string::npos);
  FUM_CHECK_EQ(status.value().attempts.size(), std::size_t{1});
  FUM_CHECK_EQ(status.value().attempts.front().state, AttemptState::abandoned);

  // The abandoned attempt can never commit: nothing further is published.
  FUM_REQUIRE_OK(harness->run());
  FUM_CHECK_EQ(harness->provenance_count(), std::size_t{0});
  FUM_CHECK_EQ(harness->adapter().counters().prepare_calls, 0ull);
}

FUM_TEST(stale_attempt_cannot_mutate_a_paused_campaign) {
  HarnessOptions options;
  options.target_count = 1;
  options.executor = ExecutorKind::thread_pool;
  options.worker_threads = 2;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  auto gate = std::make_shared<SyntheticGate>();
  harness->adapter().gate_next_activate(harness->target(0), gate);
  FUM_REQUIRE_OK(harness->start(StartOptions{}));
  gate->wait_until_reached();

  // Pause while the activation is in flight, then let it finish.
  FUM_REQUIRE_OK(harness->engine().pause(harness->campaign(), "operator hold"));
  gate->open();
  FUM_REQUIRE_OK(harness->engine().wait_for_idle());

  auto status = harness->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK_EQ(status.value().campaign.state, UpgradeState::paused);
  FUM_CHECK_EQ(count_outcome(*harness, ProvenanceOutcome::applied), std::size_t{0});
  FUM_CHECK(has_fence_decision(*harness));
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, 1ull);
  // The adapter did apply the change, but the runtime never recorded it as
  // successful state: the target must be re-observed before anything proceeds.
  FUM_CHECK_EQ(harness->adapter().version_of(harness->target(0)).text(), std::string("1.1.0"));
}

FUM_TEST(shutdown_stops_admission_and_publishes_no_further_success) {
  HarnessOptions options;
  options.target_count = 2;
  options.strategy = StrategyKind::restart_based;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  FUM_REQUIRE_OK(harness->start(StartOptions{}));
  FUM_REQUIRE_OK(harness->engine().run_steps(1));
  const auto counters_before = harness->adapter().counters();
  FUM_REQUIRE_OK(harness->engine().shutdown());

  // After shutdown, admission and execution are closed: no success is published.
  FUM_REQUIRE_ERR(harness->engine().run_until_idle(), ErrorCode::cancelled);
  FUM_REQUIRE_ERR(harness->engine().start(harness->campaign(), StartOptions{}),
                  ErrorCode::cancelled);
  FUM_REQUIRE_ERR(harness->engine().pause(harness->campaign(), "late"), ErrorCode::cancelled);
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, counters_before.activate_calls);
  FUM_CHECK_EQ(count_outcome(*harness, ProvenanceOutcome::verified), std::size_t{0});
  auto status = harness->engine().status(harness->campaign());
  FUM_CHECK(status.has_value());
  FUM_CHECK_NE(status.value().campaign.state, UpgradeState::completed);
  // Shutdown is idempotent.
  FUM_REQUIRE_OK(harness->engine().shutdown());
}

FUM_TEST(unsupported_rollback_after_a_configuration_failure_still_records_the_failure) {
  HarnessOptions options;
  options.target_count = 1;
  options.with_configuration = true;
  auto harness = Harness::create(options);
  harness->configuration().set_failure(ErrorCode::io_error);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::failed);
  auto status = harness->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK(status.value().campaign.failure_reason.find("configuration") != std::string::npos);
}

FUM_TEST(rollout_fabric_refusal_prevents_execution) {
  HarnessOptions options;
  options.target_count = 1;
  options.with_rollout = true;
  auto harness = Harness::create(options);
  harness->rollout().set_admit(false, "outside the change window");
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  FUM_REQUIRE_ERR(harness->start(StartOptions{}), ErrorCode::policy_denied);
  FUM_CHECK_EQ(harness->adapter().counters().prepare_calls, 0ull);
  FUM_CHECK_EQ(harness->state(), UpgradeState::prepared);
}

int main(int argc, char** argv) { return fumtest::run_all(argc, argv); }
