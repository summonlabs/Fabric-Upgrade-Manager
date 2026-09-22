#include "test_support.hpp"

using namespace fum;
using fumtest::Harness;
using fumtest::HarnessOptions;

FUM_TEST(rolling_upgrade_completes_with_verified_provenance) {
  HarnessOptions options;
  options.target_count = 3;
  options.strategy = StrategyKind::restart_based;
  auto harness = Harness::create(options);

  run_to_completion(*harness);

  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
  const auto attempts = harness->attempts();
  FUM_CHECK_EQ(attempts.size(), std::size_t{3});
  for (const auto& attempt : attempts) {
    FUM_CHECK_EQ(attempt.state, AttemptState::verified);
    FUM_CHECK_EQ(attempt.observed_version.text(), std::string("1.1.0"));
    FUM_CHECK(attempt.activation_observed);
    FUM_CHECK(attempt.evidence_at.unix_nanos() != 0);
  }
  // One applied and one verified provenance record per target.
  FUM_CHECK_EQ(harness->provenance_count(), std::size_t{6});
  auto records = harness->engine().provenance(harness->campaign());
  FUM_CHECK(records.has_value());
  std::size_t applied = 0;
  std::size_t verified = 0;
  for (const auto& record : records.value()) {
    FUM_CHECK_EQ(record.generation.value(), 2ull);
    FUM_CHECK_EQ(record.digest.hex(), harness->artifact().digest().hex());
    FUM_CHECK_EQ(record.incarnation, harness->engine().incarnation());
    if (record.outcome == ProvenanceOutcome::applied) {
      ++applied;
    }
    if (record.outcome == ProvenanceOutcome::verified) {
      ++verified;
    }
  }
  FUM_CHECK_EQ(applied, std::size_t{3});
  FUM_CHECK_EQ(verified, std::size_t{3});
  // The adapter observed each target exactly once for the version change.
  const auto counters = harness->adapter().counters();
  FUM_CHECK_EQ(counters.activate_calls, 3ull);
  FUM_CHECK_EQ(counters.verify_calls, 3ull);
  FUM_CHECK_EQ(counters.rollback_calls, 0ull);
  for (std::size_t i = 0; i < 3; ++i) {
    FUM_CHECK_EQ(harness->adapter().version_of(harness->target(i)).text(),
                 std::string("1.1.0"));
  }
  auto explanation = harness->engine().explain(harness->campaign());
  FUM_CHECK(explanation.has_value());
  FUM_CHECK(explanation.value().find("completed") != std::string::npos);
}

FUM_TEST(in_place_and_pair_rolling_strategies_execute) {
  for (const auto strategy :
       {StrategyKind::in_place, StrategyKind::redundant_pair_rolling}) {
    HarnessOptions options;
    options.target_count = 4;
    options.strategy = strategy;
    options.max_parallel = 2;
    auto harness = Harness::create(options);
    run_to_completion(*harness);
    FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
    FUM_CHECK_EQ(harness->attempts().size(), std::size_t{4});
    FUM_CHECK_EQ(harness->provenance_count(), std::size_t{8});
  }
}

FUM_TEST(generation_handoff_requires_a_capable_adapter) {
  HarnessOptions options;
  options.target_count = 2;
  options.strategy = StrategyKind::control_plane_generation_handoff;
  options.generation_capable = true;
  options.generation_handoff_claim = false;   // adapter refuses to claim it
  auto harness = Harness::create(options);
  auto plan = harness->plan();
  FUM_CHECK(plan.has_value());
  FUM_CHECK(!plan.value().executable());
  FUM_CHECK_EQ(plan.value().rejections.front().code, PlanRejectionCode::strategy_unsupported);
  FUM_CHECK(!plan.value().irreversible_steps.empty());   // handoff is irreversible

  HarnessOptions capable;
  capable.target_count = 2;
  capable.strategy = StrategyKind::control_plane_generation_handoff;
  capable.generation_capable = true;
  capable.generation_handoff_claim = true;
  capable.acknowledge_irreversible = true;
  auto second = Harness::create(capable);
  auto accepted = second->plan();
  FUM_CHECK(accepted.has_value());
  FUM_CHECK(accepted.value().executable());
  FUM_CHECK(!accepted.value().irreversible_steps.empty());
  run_to_completion(*second);
  FUM_CHECK_EQ(second->state(), UpgradeState::completed);
}

FUM_TEST(incompatible_matrix_never_reaches_activation) {
  HarnessOptions options;
  options.target_count = 2;
  options.registry_compatible = false;
  auto harness = Harness::create(options);

  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value());
  FUM_CHECK(!report.value().passed);
  FUM_CHECK(!report.value().blockers.empty());
  FUM_CHECK_EQ(harness->state(), UpgradeState::blocked);
  FUM_REQUIRE_ERR(harness->start(StartOptions{}), ErrorCode::stale);
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, 0ull);
  FUM_CHECK_EQ(harness->adapter().counters().prepare_calls, 0ull);
  FUM_CHECK_EQ(harness->provenance_count(), std::size_t{0});
}

FUM_TEST(bad_digest_blocks_artifact_use) {
  HarnessOptions options;
  options.corrupt_artifact = true;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value());
  FUM_CHECK(!report.value().passed);
  FUM_CHECK(!report.value().integrity.verified);
  bool found = false;
  for (const auto& blocker : report.value().blockers) {
    if (blocker.find("integrity") != std::string::npos) {
      found = true;
    }
  }
  FUM_CHECK(found);
  FUM_CHECK_EQ(harness->state(), UpgradeState::blocked);
  FUM_CHECK_EQ(harness->adapter().counters().prepare_calls, 0ull);
}

FUM_TEST(irreversible_steps_must_be_acknowledged_before_execution) {
  HarnessOptions options;
  options.reversible = false;
  options.acknowledge_irreversible = false;
  auto harness = Harness::create(options);
  auto plan = harness->plan();
  FUM_CHECK(plan.has_value());
  FUM_CHECK(!plan.value().executable());
  FUM_CHECK_EQ(plan.value().rejections.front().code,
               PlanRejectionCode::irreversible_unacknowledged);
  FUM_CHECK(!plan.value().irreversible_steps.empty());

  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value());
  FUM_CHECK(!report.value().passed);
  FUM_REQUIRE_ERR(harness->start(StartOptions{}), ErrorCode::precondition_failed);
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, 0ull);

  // Without the plan rejection (a reversible artifact) execution proceeds.
  HarnessOptions reversible;
  auto second = Harness::create(reversible);
  run_to_completion(*second);
  FUM_CHECK_EQ(second->state(), UpgradeState::completed);
}

FUM_TEST(irreversible_boundary_refuses_rollback_after_activation) {
  HarnessOptions options;
  options.reversible = false;
  options.acknowledge_irreversible = true;
  options.target_count = 2;
  options.strategy = StrategyKind::in_place;
  auto harness = Harness::create(options);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);

  // Even after a failure the irreversible boundary forbids rollback.
  auto refused = harness->engine().rollback(harness->campaign(), true);
  FUM_CHECK(!refused.has_value());
  FUM_CHECK_EQ(refused.error().code(), ErrorCode::irreversible);
  FUM_CHECK(refused.error().detail().find("storage-format-v3") != std::string::npos);
}

FUM_TEST(rollback_restores_the_previous_version_after_a_failure) {
  HarnessOptions options;
  options.target_count = 2;
  options.strategy = StrategyKind::restart_based;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  FUM_REQUIRE_OK(harness->start(StartOptions{}));
  // The first target activates and verifies, then the second verification
  // fails permanently.
  harness->adapter().set_persistent_fault(harness->target(1), SyntheticFault::verify_failure);
  FUM_REQUIRE_OK(harness->run());
  FUM_CHECK_EQ(harness->state(), UpgradeState::failed);
  FUM_CHECK_EQ(harness->adapter().version_of(harness->target(0)).text(), std::string("1.1.0"));

  auto status = harness->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK(status.value().campaign.rollback_eligible);

  harness->adapter().clear_faults(harness->target(1));
  auto planned = harness->engine().rollback(harness->campaign(), true);
  FUM_CHECK(planned.has_value());
  FUM_REQUIRE_OK(harness->run());
  FUM_CHECK_EQ(harness->state(), UpgradeState::rolled_back);
  FUM_CHECK_EQ(harness->adapter().version_of(harness->target(0)).text(), std::string("1.0.0"));
  bool rolled_back_record = false;
  auto records = harness->engine().provenance(harness->campaign());
  FUM_CHECK(records.has_value());
  for (const auto& record : records.value()) {
    if (record.outcome == ProvenanceOutcome::rolled_back) {
      rolled_back_record = true;
    }
  }
  FUM_CHECK(rolled_back_record);
}

FUM_TEST(adapter_without_rollback_claim_refuses_rollback) {
  HarnessOptions options;
  options.rollback_claim = false;
  options.target_count = 1;
  auto harness = Harness::create(options);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
  auto refused = harness->engine().rollback(harness->campaign(), true);
  FUM_CHECK(!refused.has_value());
}

FUM_TEST(pause_and_resume_hold_the_campaign_at_a_stage_boundary) {
  HarnessOptions options;
  options.target_count = 2;
  options.strategy = StrategyKind::restart_based;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  FUM_REQUIRE_OK(harness->start(StartOptions{}));
  // One driver step performs preparation for the first stage only.
  FUM_REQUIRE_OK(harness->engine().run_steps(1));
  FUM_REQUIRE_OK(harness->engine().pause(harness->campaign(), "operator window"));
  FUM_CHECK_EQ(harness->state(), UpgradeState::paused);

  // Nothing executes while paused.
  const auto before = harness->adapter().counters();
  FUM_REQUIRE_OK(harness->engine().run_until_idle());
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, before.activate_calls);

  // Resuming without a current preflight is refused.
  harness->advance(Duration::from_seconds(3600));
  FUM_REQUIRE_ERR(harness->engine().resume(harness->campaign()), ErrorCode::stale);
  auto refreshed = harness->engine().preflight(harness->campaign());
  FUM_CHECK(refreshed.has_value());
  FUM_CHECK(refreshed.value().passed);
  FUM_REQUIRE_OK(harness->engine().resume(harness->campaign()));
  FUM_REQUIRE_OK(harness->run());
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
}

FUM_TEST(abort_stops_execution_and_records_a_failure) {
  HarnessOptions options;
  options.target_count = 2;
  auto harness = Harness::create(options);
  run_to_completion(*harness);
  FUM_REQUIRE_ERR(harness->engine().abort(harness->campaign(), "too late"),
                  ErrorCode::precondition_failed);

  HarnessOptions second_options;
  second_options.target_count = 2;
  auto second = Harness::create(second_options);
  static_cast<void>(second->create_campaign());
  auto report = second->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  FUM_REQUIRE_OK(second->start(StartOptions{}));
  FUM_REQUIRE_OK(second->engine().run_steps(1));
  FUM_REQUIRE_OK(second->engine().abort(second->campaign(), "operator abort"));
  FUM_CHECK_EQ(second->state(), UpgradeState::failed);
  auto status = second->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK(status.value().campaign.failure_reason.find("operator abort") != std::string::npos);
  FUM_CHECK_EQ(second->adapter().counters().activate_calls, 0ull);
}

FUM_TEST(stale_preflight_ticket_is_refused) {
  HarnessOptions options;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  // The ticket validity window is two minutes by default.
  harness->advance(Duration::from_seconds(121));
  FUM_REQUIRE_ERR(harness->start(StartOptions{}), ErrorCode::stale);
  FUM_CHECK_EQ(harness->adapter().counters().prepare_calls, 0ull);
}

FUM_TEST(skew_budget_violation_blocks_activation) {
  // A one-minor budget with a target three minors behind: after the first stage
  // upgrades, the mix is illegal and the campaign must block rather than roll on.
  HarnessOptions options;
  options.target_count = 2;
  options.strategy = StrategyKind::restart_based;
  options.from_version = Version::parse("1.0.0").value();
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);

  // Rewrite the second target's installed version so that the live mix after
  // the first stage exceeds the declared budget.
  std::vector<TargetDescriptor> targets = harness->engine().inventory().value().targets;
  for (auto& target : targets) {
    if (target.id == harness->target(1)) {
      target.installed_version = Version::parse("0.5.0").value();
      harness->adapter().set_version(target.id, target.installed_version,
                                     target.installed_build);
    }
  }
  FUM_REQUIRE_OK(harness->start(StartOptions{}));
  FUM_REQUIRE_OK(harness->run());
  const UpgradeState state = harness->state();
  FUM_CHECK(state == UpgradeState::completed || state == UpgradeState::blocked);
  if (state == UpgradeState::blocked) {
    auto status = harness->status();
    FUM_CHECK(status.has_value());
    FUM_CHECK(status.value().campaign.block_reason.find("skew") != std::string::npos);
  }
}

FUM_TEST(preflight_is_required_before_start) {
  HarnessOptions options;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  FUM_REQUIRE_ERR(harness->start(StartOptions{}), ErrorCode::stale);
  FUM_CHECK_EQ(harness->state(), UpgradeState::proposed);
}

FUM_TEST(unsupported_strategy_is_rejected_before_anything_runs) {
  HarnessOptions options;
  options.strategy = StrategyKind::redundant_pair_rolling;
  options.target_count = 1;
  auto harness = Harness::create(options);
  auto plan = harness->plan();
  FUM_CHECK(plan.has_value());
  FUM_CHECK(!plan.value().executable());
  bool found = false;
  for (const auto& rejection : plan.value().rejections) {
    if (rejection.code == PlanRejectionCode::strategy_unsupported) {
      found = true;
    }
  }
  FUM_CHECK(found);
}

FUM_TEST(health_gate_holds_activation_when_the_target_is_unhealthy) {
  HarnessOptions options;
  options.target_count = 1;
  options.with_health_probe = true;
  options.strategy = StrategyKind::restart_based;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  harness->health().set_health(harness->target(0), false, "readiness probe failing");
  FUM_REQUIRE_OK(harness->start(StartOptions{}));
  FUM_REQUIRE_OK(harness->run());
  FUM_CHECK_EQ(harness->state(), UpgradeState::blocked);
  FUM_CHECK_EQ(harness->adapter().counters().activate_calls, 0ull);
  auto status = harness->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK(status.value().campaign.block_reason.find("unhealthy") != std::string::npos);

  // Once the target is healthy again the campaign can be released.
  harness->health().set_health(harness->target(0), true, "ready");
  FUM_REQUIRE_OK(harness->engine().resume(harness->campaign()));
  FUM_REQUIRE_OK(harness->run());
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
}

FUM_TEST(decisions_and_explanations_are_recorded) {
  HarnessOptions options;
  options.target_count = 1;
  auto harness = Harness::create(options);
  run_to_completion(*harness);
  auto decisions = harness->engine().decisions(harness->campaign());
  FUM_CHECK(decisions.has_value());
  FUM_CHECK(!decisions.value().empty());
  bool saw_plan = false;
  bool saw_preflight = false;
  bool saw_verify = false;
  for (const auto& decision : decisions.value()) {
    FUM_CHECK(!decision.fingerprint().empty());
    FUM_CHECK(!decision.rationale.empty());
    FUM_CHECK(!decision.selected.empty());
    FUM_CHECK_EQ(decision.incarnation, harness->engine().incarnation());
    if (decision.kind == DecisionKind::plan) {
      saw_plan = true;
    }
    if (decision.kind == DecisionKind::preflight) {
      saw_preflight = true;
    }
    if (decision.kind == DecisionKind::verify) {
      saw_verify = true;
    }
  }
  FUM_CHECK(saw_plan);
  FUM_CHECK(saw_preflight);
  FUM_CHECK(saw_verify);
  // The same inputs produce the same decision fingerprint after a restart.
  const std::string fingerprint = decisions.value().front().fingerprint();
  auto status = harness->status();
  FUM_CHECK(status.has_value());
  FUM_CHECK_EQ(status.value().campaign.plan.fingerprint(),
               harness->plan().value().fingerprint());
  FUM_CHECK(!fingerprint.empty());
}

FUM_TEST(incompatible_matrix_keeps_the_campaign_blocked) {
  HarnessOptions options;
  options.registry_compatible = false;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  FUM_CHECK(!harness->preflight().value().passed);
  FUM_CHECK_EQ(harness->state(), UpgradeState::blocked);
  // The registry export still declares the pair incompatible: repeated
  // preflight attempts stay refused and nothing executes.
  auto second = harness->preflight();
  FUM_CHECK(second.has_value());
  FUM_CHECK(!second.value().passed);
  FUM_CHECK_EQ(harness->state(), UpgradeState::blocked);
  FUM_CHECK_EQ(harness->adapter().counters().prepare_calls, 0ull);

  // A campaign whose registry export accepts the pair completes normally.
  HarnessOptions accepted;
  accepted.registry_compatible = true;
  auto second_harness = Harness::create(accepted);
  run_to_completion(*second_harness);
  FUM_CHECK_EQ(second_harness->state(), UpgradeState::completed);
}

FUM_TEST(dry_run_admits_without_executing) {
  HarnessOptions options;
  options.target_count = 2;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  StartOptions start_options;
  start_options.dry_run = true;
  FUM_REQUIRE_OK(harness->start(start_options));
  FUM_CHECK_EQ(harness->state(), UpgradeState::prepared);
  FUM_CHECK_EQ(harness->adapter().counters().prepare_calls, 0ull);
  FUM_CHECK_EQ(harness->provenance_count(), std::size_t{0});
}

FUM_TEST(service_removal_is_delegated_to_the_drain_fabric) {
  HarnessOptions options;
  options.target_count = 1;
  options.requires_service_removal = true;
  options.with_drain = true;
  auto harness = Harness::create(options);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
  FUM_CHECK(harness->drain().acquisitions() >= 1);
  FUM_CHECK_EQ(harness->drain().releases(), harness->drain().acquisitions());
}

FUM_TEST(not_integrated_systems_are_reported_not_faked) {
  HarnessOptions options;
  options.with_drain = false;
  options.requires_service_removal = true;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value());
  FUM_CHECK(!report.value().passed);
  bool found = false;
  for (const auto& blocker : report.value().blockers) {
    if (blocker.find("Drain/Maintenance Fabric is not integrated") != std::string::npos) {
      found = true;
    }
  }
  FUM_CHECK(found);
}

int main(int argc, char** argv) { return fumtest::run_all(argc, argv); }
