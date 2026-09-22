// Adversarial inputs: malformed, truncated, corrupt and hostile data.
#include <future>
#include <random>

#include "test_support.hpp"

#include "fum/store/journal.hpp"
#include "fum/store/state.hpp"

using namespace fum;
using fumtest::Harness;
using fumtest::HarnessOptions;

namespace {

std::string mutate(const std::string& original, std::mt19937& generator) {
  std::string copy = original;
  if (copy.empty()) {
    return copy;
  }
  std::uniform_int_distribution<std::size_t> position(0, copy.size() - 1);
  std::uniform_int_distribution<int> byte(0, 255);
  const std::size_t count = 1 + (generator() % 3);
  for (std::size_t i = 0; i < count; ++i) {
    copy[position(generator)] = static_cast<char>(byte(generator));
  }
  return copy;
}

}  // namespace

FUM_TEST(random_json_mutations_never_crash_the_parser) {
  const std::string document =
      "{\"a\":[1,2,3,{\"b\":\"text\",\"c\":null,\"d\":true}],\"e\":-1.5e3,\"f\":"
      "\"\\u00e9\"}";
  auto baseline = json::parse(document);
  FUM_CHECK(baseline.has_value());
  std::mt19937 generator(20260101);
  std::size_t parsed_ok = 0;
  std::size_t rejected = 0;
  for (int iteration = 0; iteration < 4000; ++iteration) {
    const std::string mutated = mutate(document, generator);
    auto result = json::parse(mutated);
    if (result.has_value()) {
      ++parsed_ok;
      // Anything that parses must also re-serialize and re-parse identically.
      const std::string dumped = result.value().dump();
      auto again = json::parse(dumped);
      FUM_CHECK(again.has_value());
      FUM_CHECK(again.value().dump() == dumped);
    } else {
      ++rejected;
      FUM_CHECK(!result.error().message().empty());
    }
  }
  FUM_CHECK(parsed_ok + rejected == 4000);
  FUM_CHECK(rejected > 0);
}

FUM_TEST(random_identifiers_and_versions_are_validated) {
  std::mt19937 generator(4242);
  std::uniform_int_distribution<int> character(0, 255);
  std::uniform_int_distribution<int> length(0, 200);
  for (int iteration = 0; iteration < 4000; ++iteration) {
    std::string text;
    const int size = length(generator);
    for (int i = 0; i < size; ++i) {
      text.push_back(static_cast<char>(character(generator)));
    }
    auto id = ComponentId::parse(text);
    if (id.has_value()) {
      FUM_CHECK(!id.value().str().empty());
      FUM_CHECK_EQ(id.value().str().size(), text.size());
    }
    auto version = Version::parse(text);
    if (version.has_value()) {
      FUM_CHECK(!version.value().text().empty());
      FUM_CHECK_EQ(version.value().text().size(), text.size());
    }
    auto digest = Digest::parse(text);
    if (digest.has_value()) {
      FUM_CHECK(digest.value().hex().empty() || digest.value().hex().size() == 64);
    }
    auto timestamp = Timestamp::parse_iso8601(text);
    if (timestamp.has_value()) {
      FUM_CHECK(Timestamp::parse_iso8601(timestamp.value().to_iso8601()).has_value());
    }
  }
}

FUM_TEST(absurd_artifact_metadata_is_refused) {
  ArtifactDescriptor artifact;
  artifact.set_id(make_ArtifactId("artifact-1"));
  artifact.set_component(make_ComponentId("fabric-core"));
  artifact.set_version(Version::parse("1.0.0").value());
  artifact.set_build(make_BuildId("build-1"));
  artifact.set_digest(Digest::of_bytes("payload"));
  artifact.set_platforms({"linux/x86_64"});
  RollbackCompatibility rollback;
  rollback.reversible = true;
  artifact.set_rollback(rollback);

  artifact.set_size_bytes(UINT64_MAX);
  FUM_REQUIRE_ERR(artifact.validate(), ErrorCode::resource_exhausted);
  artifact.set_size_bytes(0);
  FUM_REQUIRE_ERR(artifact.validate(), ErrorCode::invalid_argument);
  artifact.set_size_bytes(7);
  FUM_CHECK(artifact.validate().has_value());

  json::Value document = artifact.to_json();
  document.set("size_bytes", json::Value::make_string("not a number"));
  FUM_CHECK(!ArtifactDescriptor::from_json(document).has_value());
  document = artifact.to_json();
  document.set("size_bytes", json::Value::make_int(-1));
  FUM_CHECK(!ArtifactDescriptor::from_json(document).has_value());
  document = artifact.to_json();
  document.set("platforms", json::Value::make_string("linux"));
  FUM_CHECK(!ArtifactDescriptor::from_json(document).has_value());
  document = artifact.to_json();
  document.set("version", json::Value::make_string("not-a-version"));
  FUM_CHECK(!ArtifactDescriptor::from_json(document).has_value());
  document = artifact.to_json();
  document.set("digest", json::Value::make_string(std::string(1000, 'a')));
  FUM_CHECK(!ArtifactDescriptor::from_json(document).has_value());
  document = artifact.to_json();
  document.set("provenance", [] {
    json::Value value = json::Value::make_object();
    value.set("signer", json::Value::make_string(""));
    value.set("key_id", json::Value::make_string("k"));
    value.set("algorithm", json::Value::make_string(""));
    return value;
  }());
  auto undeclared = ArtifactDescriptor::from_json(document);
  FUM_CHECK(undeclared.has_value());
  FUM_CHECK(!undeclared.value().provenance().declared());
}

FUM_TEST(journal_header_corruption_is_detected) {
  auto directory = fs::TempDir::create("fum-adversarial-journal");
  FUM_CHECK(directory.has_value());
  fs::TempDir guard = std::move(directory).value();
  guard.keep();
  const std::string path = fs::join(guard.path(), "journal.fum");
  {
    auto journal = Journal::open(path);
    FUM_CHECK(journal.has_value());
    json::Value payload = json::Value::make_object();
    payload.set("value", json::Value::make_int(1));
    FUM_REQUIRE_OK(journal.value().append(JournalRecordType::note, payload, Timestamp{}));
    FUM_REQUIRE_OK(journal.value().close());
  }
  const std::string good = fs::read_file(path).value();
  FUM_CHECK(good.size() > 32);

  // Wrong magic.
  std::string bad_magic = good;
  bad_magic[0] = 'X';
  FUM_REQUIRE_OK(fs::write_file(path, bad_magic));
  FUM_REQUIRE_ERR(Journal::open(path), ErrorCode::corrupt);

  // Unsupported version.
  std::string bad_version = good;
  bad_version[8] = static_cast<char>(0x7F);
  FUM_REQUIRE_OK(fs::write_file(path, bad_version));
  FUM_REQUIRE_ERR(Journal::open(path), ErrorCode::corrupt);

  // Truncated below the header size.
  FUM_REQUIRE_OK(fs::write_file(path, good.substr(0, 8)));
  FUM_REQUIRE_ERR(Journal::open(path), ErrorCode::corrupt);

  // Every truncation of a valid journal either opens with a damaged tail or
  // fails cleanly; it never crashes or silently accepts damage.
  for (std::size_t cut = 21; cut < good.size(); cut += 7) {
    FUM_REQUIRE_OK(fs::write_file(path, good.substr(0, cut)));
    auto journal = Journal::open(path);
    if (journal.has_value()) {
      FUM_CHECK(journal.value().recovery().truncated_tail || cut == good.size());
    }
  }
  FUM_REQUIRE_OK(fs::write_file(path, good));
  auto repaired = Journal::open(path);
  FUM_CHECK(repaired.has_value());
  FUM_CHECK(!repaired.value().recovery().truncated_tail);
}

FUM_TEST(corrupt_record_payloads_are_rejected_not_trusted) {
  auto directory = fs::TempDir::create("fum-adversarial-store");
  FUM_CHECK(directory.has_value());
  fs::TempDir guard = std::move(directory).value();
  guard.keep();
  StoreOptions options;
  options.directory = guard.path();
  const CampaignId campaign_id = make_CampaignId("campaign-adv");
  {
    auto store = DurableStore::open(options);
    FUM_CHECK(store.has_value());
    CampaignRecord campaign;
    campaign.id = campaign_id;
    campaign.generation = Generation(1);
    campaign.epoch = Epoch(1);
    campaign.authority = make_AuthorityId("authority");
    campaign.component = make_ComponentId("fabric-core");
    campaign.created_at = Timestamp::from_unix_nanos(1000);
    campaign.updated_at = Timestamp::from_unix_nanos(1000);
    FUM_REQUIRE_OK(store.value().put_campaign(campaign, Timestamp::from_unix_nanos(1000)));
    FUM_REQUIRE_OK(store.value().close());
  }
  const std::string path = fs::join(guard.path(), "journal.fum");
  std::string bytes = fs::read_file(path).value();
  // Corrupt the campaign payload in a way that keeps the frame length valid but
  // breaks the structure: the record must be rejected, not applied.
  const std::size_t payload = bytes.find("\"campaign_id\"");
  FUM_CHECK(payload != std::string::npos);
  bytes[payload + 5] = 'Z';
  FUM_REQUIRE_OK(fs::write_file(path, bytes));

  // The damage is reported, never silently repaired.
  auto refused = DurableStore::open(options);
  FUM_CHECK(!refused.has_value());
  FUM_CHECK_EQ(refused.error().code(), ErrorCode::corrupt);
  FUM_CHECK(refused.error().detail().find("offset") != std::string::npos);

  // With explicit authorisation the damaged record is dropped, not applied.
  StoreOptions repair = options;
  repair.repair_damaged_tail = true;
  auto store = DurableStore::open(repair);
  FUM_CHECK(store.has_value());
  FUM_CHECK_EQ(store.value().campaigns().size(), std::size_t{0});
  FUM_CHECK_EQ(store.value().recovery().rejected_records, std::size_t{0});
  FUM_REQUIRE_OK(store.value().close());
}

FUM_TEST(hostile_inventory_and_registry_documents_are_refused) {
  HarnessOptions options;
  options.target_count = 1;
  auto harness = Harness::create(options);
  // A target with an empty authority is refused by the model.
  TargetDescriptor broken = harness->engine().inventory().value().targets.front();
  broken.authority = AuthorityId{};
  FUM_REQUIRE_ERR(broken.validate(), ErrorCode::invalid_argument);
  broken = harness->engine().inventory().value().targets.front();
  broken.adapter = AdapterId{};
  FUM_REQUIRE_ERR(broken.validate(), ErrorCode::invalid_argument);
  broken = harness->engine().inventory().value().targets.front();
  broken.platform.clear();
  FUM_REQUIRE_ERR(broken.validate(), ErrorCode::invalid_argument);
  broken = harness->engine().inventory().value().targets.front();
  broken.claims.strategies.clear();
  FUM_REQUIRE_ERR(broken.validate(), ErrorCode::invalid_argument);

  // An inventory source that fails is reported, never silently empty. The
  // cached inventory is reused only while it is fresh.
  harness->inventory().set_resolve_failure(ErrorCode::io_error);
  harness->advance(Duration::from_seconds(600));
  auto plan = harness->plan();
  FUM_CHECK(!plan.has_value());
  FUM_CHECK_EQ(plan.error().code(), ErrorCode::io_error);

  // A registry that refuses to answer blocks the campaign.
  harness->inventory().set_resolve_failure(ErrorCode::ok);
  harness->registry().set_answered(false);
  static_cast<void>(harness->create_campaign());
  auto report = harness->preflight();
  FUM_CHECK(report.has_value());
  FUM_CHECK(!report.value().passed);
  harness->registry().set_query_failure(ErrorCode::timeout);
  auto second = harness->preflight();
  FUM_CHECK(second.has_value());
  FUM_CHECK(!second.value().passed);
  FUM_REQUIRE_OK(harness->engine().shutdown());
}

FUM_TEST(bounded_resources_are_enforced_end_to_end) {
  HarnessOptions options;
  options.target_count = 2;
  auto harness = Harness::create(options);
  // A worker-free pool refuses work beyond its bound.
  WorkerPool::Options pool_options;
  pool_options.threads = 1;
  pool_options.max_queue = 1;
  WorkerPool pool(pool_options);
  std::atomic<bool> release{false};
  std::promise<void> started;
  auto started_future = started.get_future();
  FUM_REQUIRE_OK(pool.submit([&release, &started] {
    started.set_value();
    while (!release.load()) {
      std::this_thread::yield();
    }
  }));
  started_future.wait();   // the worker is busy, so one queue slot remains
  FUM_REQUIRE_OK(pool.submit([] {}));
  FUM_REQUIRE_ERR(pool.submit([] {}), ErrorCode::resource_exhausted);
  release.store(true);
  FUM_REQUIRE_OK(pool.drain());
  pool.stop();

  // Planning refuses absurd target counts.
  UpgradePlanRequest request = harness->request();
  request.targets.assign(600, harness->target(0));
  auto plan = harness->engine().plan(request);
  FUM_CHECK(plan.has_value());
  FUM_CHECK(!plan.value().executable());
  FUM_REQUIRE_OK(harness->engine().shutdown());
}

int main(int argc, char** argv) { return fumtest::run_all(argc, argv); }
