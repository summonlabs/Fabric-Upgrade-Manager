// Concurrency: real threads, bounded queues, cancellation and shutdown.
#include <atomic>
#include <future>
#include <thread>
#include <vector>

#include "test_support.hpp"

using namespace fum;
using fumtest::Harness;
using fumtest::HarnessOptions;

FUM_TEST(worker_pool_bounds_and_accounting) {
  WorkerPool::Options options;
  options.threads = 2;
  options.max_queue = 4;
  WorkerPool pool(options);
  std::atomic<int> executed{0};
  std::atomic<int> rejected{0};
  const std::function<void()> task = [&executed] { executed.fetch_add(1); };
  for (int i = 0; i < 64; ++i) {
    auto status = pool.submit(task);
    if (!status.has_value()) {
      ++rejected;
      FUM_CHECK_EQ(status.error().code(), ErrorCode::resource_exhausted);
    }
  }
  FUM_REQUIRE_OK(pool.drain());
  const auto stats = pool.stats();
  FUM_CHECK_EQ(stats.submitted, stats.completed);
  FUM_CHECK_EQ(static_cast<int>(stats.rejected), rejected.load());
  FUM_CHECK_EQ(static_cast<int>(stats.completed + stats.rejected), 64);
  FUM_CHECK_EQ(executed.load(), static_cast<int>(stats.completed));
  FUM_CHECK_EQ(pool.pending(), std::size_t{0});
  FUM_CHECK_EQ(pool.active(), std::size_t{0});
  pool.stop();
  FUM_CHECK(pool.stopped());
  FUM_REQUIRE_ERR(pool.submit(task), ErrorCode::closed);
  pool.stop();   // idempotent
}

FUM_TEST(worker_pool_refuses_a_self_join) {
  WorkerPool::Options options;
  options.threads = 1;
  options.max_queue = 2;
  WorkerPool pool(options);
  std::atomic<bool> refused{false};
  FUM_REQUIRE_OK(pool.submit([&pool, &refused] {
    const Status status = pool.drain();
    refused.store(!status.has_value() && status.error().code() == ErrorCode::internal);
  }));
  FUM_REQUIRE_OK(pool.drain());
  FUM_CHECK(refused.load());
  pool.stop();
}

FUM_TEST(cancellation_discards_queued_work_without_executing_it) {
  WorkerPool::Options options;
  options.threads = 1;
  options.max_queue = 16;
  WorkerPool pool(options);
  std::atomic<int> executed{0};
  std::atomic<bool> release{false};
  std::promise<void> started;
  auto started_future = started.get_future();
  FUM_REQUIRE_OK(pool.submit([&release, &started] {
    started.set_value();
    while (!release.load()) {
      std::this_thread::yield();
    }
  }));
  // The worker is provably busy with the first task, so the rest are queued.
  started_future.wait();
  for (int i = 0; i < 8; ++i) {
    static_cast<void>(pool.submit([&executed] { executed.fetch_add(1); }));
  }
  pool.request_cancel();
  release.store(true);
  FUM_REQUIRE_OK(pool.drain());
  const auto stats = pool.stats();
  FUM_CHECK_EQ(stats.discarded, 8ull);
  FUM_CHECK_EQ(executed.load(), 0);
  FUM_CHECK_EQ(stats.completed, 1ull);   // only the blocking task ran
  pool.stop();
}

FUM_TEST(engine_thread_pool_runs_campaigns_and_keeps_the_ledger_valid) {
  HarnessOptions options;
  options.target_count = 4;
  options.strategy = StrategyKind::in_place;
  options.max_parallel = 4;
  options.executor = ExecutorKind::thread_pool;
  options.worker_threads = 4;
  auto harness = Harness::create(options);
  std::vector<CampaignId> campaigns;
  for (int i = 0; i < 4; ++i) {
    UpgradePlanRequest request = harness->request();
    request.campaign = make_CampaignId("concurrent-" + std::to_string(i));
    auto created = harness->engine().create_campaign(request);
    FUM_CHECK(created.has_value());
    auto report = harness->engine().preflight(request.campaign);
    FUM_CHECK(report.has_value() && report.value().passed);
    FUM_REQUIRE_OK(harness->engine().start(request.campaign, StartOptions{}));
    campaigns.push_back(request.campaign);
  }
  FUM_REQUIRE_OK(harness->engine().wait_for_idle());
  for (const auto& campaign : campaigns) {
    auto status = harness->engine().status(campaign);
    FUM_CHECK(status.has_value());
    FUM_CHECK_EQ(status.value().campaign.state, UpgradeState::completed);
  }
  auto records = harness->engine().provenance(CampaignId{});
  FUM_CHECK(records.has_value());
  FUM_CHECK_EQ(records.value().size(), std::size_t{4 * 4 * 2});
  FUM_CHECK(verify_ledger(records.value()).valid);
  FUM_REQUIRE_OK(harness->engine().shutdown());
}

FUM_TEST(concurrent_readers_observe_consistent_state) {
  HarnessOptions options;
  options.target_count = 3;
  options.strategy = StrategyKind::restart_based;
  options.executor = ExecutorKind::thread_pool;
  options.worker_threads = 3;
  auto harness = Harness::create(options);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value() && report.value().passed);
  auto gate = std::make_shared<SyntheticGate>();
  harness->adapter().gate_next_activate(harness->target(0), gate);
  FUM_REQUIRE_OK(harness->start(StartOptions{}));
  gate->wait_until_reached();

  std::atomic<bool> stop{false};
  std::atomic<int> reads{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&harness, &stop, &reads] {
      while (!stop.load()) {
        auto status = harness->engine().status(harness->campaign());
        if (status.has_value()) {
          reads.fetch_add(1);
          // Observed states are always part of the lifecycle.
          const UpgradeState state = status.value().campaign.state;
          const bool known = state == UpgradeState::proposed || state == UpgradeState::validated ||
                             state == UpgradeState::prepared || state == UpgradeState::staged ||
                             state == UpgradeState::activating || state == UpgradeState::verifying ||
                             state == UpgradeState::completed || state == UpgradeState::blocked ||
                             state == UpgradeState::paused || state == UpgradeState::failed ||
                             state == UpgradeState::rollback_planned ||
                             state == UpgradeState::rolling_back ||
                             state == UpgradeState::rolled_back;
          if (!known) {
            std::abort();
          }
        }
        std::this_thread::yield();
      }
    });
  }
  gate->open();
  FUM_REQUIRE_OK(harness->engine().wait_for_idle());
  stop.store(true);
  for (auto& reader : readers) {
    reader.join();
  }
  FUM_CHECK(reads.load() > 0);
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
  FUM_REQUIRE_OK(harness->engine().shutdown());
}

FUM_TEST(repeated_open_and_close_cycles_stay_consistent) {
  HarnessOptions options;
  options.target_count = 2;
  auto harness = Harness::create(options);
  run_to_completion(*harness);
  FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
  for (int cycle = 0; cycle < 5; ++cycle) {
    FUM_REQUIRE_OK(harness->reopen());
    FUM_CHECK_EQ(harness->engine().incarnation().value(), static_cast<std::uint64_t>(cycle + 2));
    FUM_CHECK_EQ(harness->state(), UpgradeState::completed);
    auto records = harness->engine().provenance(harness->campaign());
    FUM_CHECK(records.has_value());
    FUM_CHECK_EQ(records.value().size(), std::size_t{4});
    FUM_CHECK(verify_ledger(records.value()).valid);
    FUM_CHECK_EQ(harness->engine().recovery().rejected_records, std::size_t{0});
  }
  FUM_REQUIRE_OK(harness->engine().shutdown());
}

FUM_TEST(many_sequential_campaigns_keep_the_store_consistent) {
  HarnessOptions options;
  options.target_count = 2;
  options.strategy = StrategyKind::restart_based;
  auto harness = Harness::create(options);
  const Version artifact_version = options.to_version;
  Version installed = options.from_version;
  for (int cycle = 0; cycle < 6; ++cycle) {
    // Alternate between the two versions so every campaign has real work.
    const Version target_version = cycle % 2 == 0 ? artifact_version : options.from_version;
    for (std::size_t index = 0; index < options.target_count; ++index) {
      TargetDescriptor descriptor;
      descriptor.id = harness->target(index);
      descriptor.component = options.component;
      descriptor.display_name = descriptor.id.str();
      descriptor.installed_version = installed;
      descriptor.installed_build = cycle % 2 == 0 ? options.from_build : options.to_build;
      descriptor.platform = options.platform;
      descriptor.capabilities = {"hot-restart"};
      descriptor.adapter = harness->adapter().id();
      descriptor.adapter_kind = AdapterKind::synthetic;
      descriptor.claims = harness->adapter().claims();
      descriptor.authority = make_AuthorityId("fabric-authority");
      harness->inventory().add_target(descriptor);
    }
    UpgradePlanRequest request = harness->request();
    request.campaign = make_CampaignId("cycle-" + std::to_string(cycle));
    request.artifact = harness->artifact();
    if (cycle % 2 == 1) {
      // A reverse campaign: the artifact stays the newer version, so instead we
      // simply skip the cycle rather than inventing a downgrade plan.
      installed = target_version;
      continue;
    }
    auto created = harness->engine().create_campaign(request);
    FUM_CHECK(created.has_value());
    auto report = harness->engine().preflight(request.campaign);
    FUM_CHECK(report.has_value());
    if (!report.value().passed) {
      installed = target_version;
      continue;
    }
    FUM_REQUIRE_OK(harness->engine().start(request.campaign, StartOptions{}));
    FUM_REQUIRE_OK(harness->engine().run_until_idle());
    auto status = harness->engine().status(request.campaign);
    FUM_CHECK(status.has_value());
    FUM_CHECK_EQ(status.value().campaign.state, UpgradeState::completed);
    installed = target_version;
  }
  auto records = harness->engine().provenance(CampaignId{});
  FUM_CHECK(records.has_value());
  FUM_CHECK(verify_ledger(records.value()).valid);
  FUM_CHECK_EQ(harness->engine().recovery().rejected_records, std::size_t{0});
  FUM_REQUIRE_OK(harness->engine().shutdown());
}

int main(int argc, char** argv) { return fumtest::run_all(argc, argv); }