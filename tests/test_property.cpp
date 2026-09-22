// Deterministic property and seeded randomized state-machine tests.
#include <random>

#include "test_support.hpp"

using namespace fum;
using fumtest::Harness;
using fumtest::HarnessOptions;

namespace {

// Invariants that must hold after every driver step.
void check_invariants(Harness& harness, const std::vector<UpgradeState>& history) {
  auto status = harness.engine().status(harness.campaign());
  FUM_CHECK(status.has_value());
  const CampaignRecord& campaign = status.value().campaign;

  // A completed campaign has every stage completed and every attempt verified.
  if (campaign.state == UpgradeState::completed) {
    for (const auto& stage : campaign.stages) {
      FUM_CHECK_EQ(stage.state, StageState::completed);
    }
    for (const auto& attempt : status.value().attempts) {
      const bool acceptable = attempt.state == AttemptState::verified ||
                              attempt.state == AttemptState::abandoned ||
                              attempt.state == AttemptState::rolled_back;
      FUM_CHECK(acceptable);
      if (attempt.state == AttemptState::verified) {
        FUM_CHECK(attempt.evidence_at.unix_nanos() != 0);
        FUM_CHECK(attempt.activation_observed);
      }
    }
  }
  // Activation is never recorded without an activation observation.
  for (const auto& attempt : status.value().attempts) {
    if (attempt.state == AttemptState::verified) {
      FUM_CHECK(attempt.activation_observed);
    }
  }
  // The provenance ledger is always verifiable and strictly ordered.
  auto records = harness.engine().provenance(CampaignId{});
  FUM_CHECK(records.has_value());
  const LedgerVerification verification = verify_ledger(records.value());
  FUM_CHECK(verification.valid);
  for (const auto& record : records.value()) {
    FUM_CHECK_EQ(record.campaign.str(), campaign.id.str());
    FUM_CHECK(record.sequence.value() > 0);
  }
  // Terminal states are final.
  if (history.size() >= 2) {
    const UpgradeState previous = history[history.size() - 2];
    const UpgradeState current = history.back();
    if (upgrade_state_is_terminal(previous)) {
      FUM_CHECK_EQ(current, previous);
    }
  }
  // Blocked or paused campaigns never have work in flight.
  if (campaign.state == UpgradeState::blocked || campaign.state == UpgradeState::paused ||
      campaign.state == UpgradeState::failed || campaign.state == UpgradeState::completed ||
      campaign.state == UpgradeState::rolled_back) {
    FUM_CHECK_EQ(status.value().in_flight, std::size_t{0});
  }
}

}  // namespace

FUM_TEST(randomized_state_machine_preserves_invariants) {
  for (std::uint32_t seed = 1; seed <= 12; ++seed) {
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> target_distribution(1, 4);
    std::uniform_int_distribution<int> action_distribution(0, 9);
    std::uniform_int_distribution<int> fault_distribution(0, 6);

    HarnessOptions options;
    options.target_count = static_cast<std::size_t>(target_distribution(generator));
    options.strategy = seed % 3 == 0   ? StrategyKind::in_place
                       : seed % 3 == 1 ? StrategyKind::restart_based
                                       : StrategyKind::redundant_pair_rolling;
    options.max_parallel = 2;
    options.registry_compatible = seed % 5 != 0;
    options.reversible = seed % 4 != 0;
    options.acknowledge_irreversible = true;
    auto harness = Harness::create(options);

    std::vector<UpgradeState> history;
    auto created = harness->engine().create_campaign(harness->request());
    FUM_CHECK(created.has_value());

    // Randomly inject faults before the run starts.
    for (std::size_t index = 0; index < options.target_count; ++index) {
      if (fault_distribution(generator) == 0) {
        harness->adapter().enqueue_fault(harness->target(index), SyntheticFault::prepare_failure);
      } else if (fault_distribution(generator) == 1) {
        harness->adapter().enqueue_fault(harness->target(index),
                                         SyntheticFault::activate_timeout_effect);
      } else if (fault_distribution(generator) == 2) {
        harness->adapter().enqueue_fault(harness->target(index), SyntheticFault::verify_stale);
      } else if (fault_distribution(generator) == 3) {
        harness->adapter().enqueue_fault(harness->target(index), SyntheticFault::activate_failure);
      }
    }

    auto report = harness->engine().preflight(harness->campaign());
    FUM_CHECK(report.has_value());
    bool started = false;
    if (report.value().passed) {
      auto start = harness->engine().start(harness->campaign(), StartOptions{});
      FUM_CHECK(start.has_value());
      started = true;
    } else {
      FUM_CHECK_EQ(harness->state(), UpgradeState::blocked);
    }

    for (int step = 0; step < 40; ++step) {
      auto status = harness->engine().status(harness->campaign());
      FUM_CHECK(status.has_value());
      history.push_back(status.value().campaign.state);
      const int roll = action_distribution(generator);
      if (started && roll == 0) {
        const Status paused = harness->engine().pause(harness->campaign(), "random pause");
        FUM_CHECK(paused.has_value() || paused.error().code() == ErrorCode::precondition_failed);
      } else if (started && roll == 1) {
        const Status resumed = harness->engine().resume(harness->campaign());
        FUM_CHECK(resumed.has_value() || resumed.error().code() == ErrorCode::stale ||
                  resumed.error().code() == ErrorCode::precondition_failed);
      } else if (started && roll == 2) {
        static_cast<void>(harness->engine().preflight(harness->campaign()));
      } else if (started && roll == 3) {
        const Status aborted = harness->engine().abort(harness->campaign(), "random abort");
        FUM_CHECK(aborted.has_value() || aborted.error().code() == ErrorCode::precondition_failed);
      } else {
        const Status ran = harness->engine().run_steps(2);
        FUM_CHECK(ran.has_value());
      }
      auto current = harness->engine().status(harness->campaign());
      FUM_CHECK(current.has_value());
      history.back() = current.value().campaign.state;
      check_invariants(*harness, history);
      if (upgrade_state_is_terminal(current.value().campaign.state)) {
        break;
      }
    }

    // Whatever happened, the store stays consistent and reopens cleanly.
    FUM_CHECK_EQ(harness->engine().recovery().rejected_records, std::size_t{0});
    FUM_REQUIRE_OK(harness->reopen());
    auto after = harness->engine().status(harness->campaign());
    FUM_CHECK(after.has_value());
    auto records = harness->engine().provenance(harness->campaign());
    FUM_CHECK(records.has_value());
    FUM_CHECK(verify_ledger(records.value()).valid);
    FUM_REQUIRE_OK(harness->engine().shutdown());
  }
}

FUM_TEST(plans_and_decisions_are_deterministic_across_instances) {
  HarnessOptions options;
  options.target_count = 3;
  options.strategy = StrategyKind::restart_based;
  auto first = Harness::create(options);
  auto second = Harness::create(options);
  auto first_plan = first->plan();
  auto second_plan = second->plan();
  FUM_CHECK(first_plan.has_value() && second_plan.has_value());
  FUM_CHECK_EQ(first_plan.value().fingerprint(), second_plan.value().fingerprint());
  FUM_CHECK(first_plan.value().to_json().dump() == second_plan.value().to_json().dump());

  static_cast<void>(first->create_campaign());
  static_cast<void>(second->create_campaign());
  auto first_decisions = first->engine().decisions(first->campaign());
  auto second_decisions = second->engine().decisions(second->campaign());
  FUM_CHECK(first_decisions.has_value() && second_decisions.has_value());
  FUM_CHECK_EQ(first_decisions.value().size(), second_decisions.value().size());
  FUM_CHECK_EQ(first_decisions.value().front().fingerprint(),
               second_decisions.value().front().fingerprint());
  // The decision fingerprints survive a restart: they contain no volatile data.
  const std::string fingerprint = first_decisions.value().back().fingerprint();
  FUM_REQUIRE_OK(first->reopen());
  auto replayed = first->engine().decisions(first->campaign());
  FUM_CHECK(replayed.has_value());
  FUM_CHECK(!replayed.value().empty());
  const std::string replayed_fingerprint = replayed.value().back().fingerprint();
  FUM_CHECK_EQ(fingerprint, replayed_fingerprint);
  FUM_REQUIRE_OK(first->engine().shutdown());
}

FUM_TEST(preflight_refusal_never_executes_anything) {
  for (int margin = 0; margin < 4; ++margin) {
    HarnessOptions options;
    options.target_count = 2;
    options.registry_compatible = false;
    auto harness = Harness::create(options);
    static_cast<void>(harness->create_campaign());
    auto report = harness->engine().preflight(harness->campaign());
    FUM_CHECK(report.has_value());
    FUM_CHECK(!report.value().passed);
    FUM_REQUIRE_OK(harness->run());
    const auto counters = harness->adapter().counters();
    FUM_CHECK_EQ(counters.prepare_calls, 0ull);
    FUM_CHECK_EQ(counters.activate_calls, 0ull);
    FUM_CHECK_EQ(counters.rollback_calls, 0ull);
    FUM_CHECK_EQ(harness->provenance_count(), std::size_t{0});
    FUM_REQUIRE_OK(harness->engine().shutdown());
  }
}

FUM_TEST(skew_is_never_exceeded_while_a_campaign_runs) {
  for (std::uint32_t seed = 1; seed <= 6; ++seed) {
    HarnessOptions options;
    options.target_count = 4;
    options.strategy = StrategyKind::restart_based;
    auto harness = Harness::create(options);
    static_cast<void>(harness->create_campaign());
    auto report = harness->engine().preflight(harness->campaign());
    FUM_CHECK(report.has_value() && report.value().passed);
    FUM_REQUIRE_OK(harness->start(StartOptions{}));
    for (int step = 0; step < 12; ++step) {
      FUM_REQUIRE_OK(harness->engine().run_steps(1));
      auto status = harness->engine().status(harness->campaign());
      FUM_CHECK(status.has_value());
      // The recorded skew assessment never exceeds the declared budget unless
      // the campaign is blocked at that moment.
      if (status.value().campaign.state == UpgradeState::completed) {
        break;
      }
      if (!status.value().skew.within_budget) {
        FUM_CHECK_EQ(status.value().campaign.state, UpgradeState::blocked);
      }
    }
    FUM_REQUIRE_OK(harness->engine().shutdown());
  }
}

int main(int argc, char** argv) { return fumtest::run_all(argc, argv); }
