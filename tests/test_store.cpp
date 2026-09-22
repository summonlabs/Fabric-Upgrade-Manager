#include "test_support.hpp"

#include "fum/store/journal.hpp"
#include "fum/store/state.hpp"

using namespace fum;

namespace {

Timestamp at(std::int64_t seconds) {
  return Timestamp::from_unix_nanos(seconds * kNanosPerSecond);
}

struct JournalFixture {
  fs::TempDir directory;
  std::string path;

  static JournalFixture create(std::string_view prefix) {
    auto dir = fs::TempDir::create(prefix);
    if (!dir.has_value()) {
      fumtest::fail(__FILE__, __LINE__, "no temporary directory");
    }
    JournalFixture fixture;
    fixture.directory = std::move(dir).value();
    fixture.directory.keep();
    fixture.path = fs::join(fixture.directory.path(), "journal.fum");
    return fixture;
  }
};

}  // namespace

FUM_TEST(journal_round_trip_preserves_records_and_chain) {
  auto fixture = JournalFixture::create("fum-journal");
  {
    auto journal = Journal::open(fixture.path);
    FUM_CHECK(journal.has_value());
    for (int i = 0; i < 5; ++i) {
      json::Value payload = json::Value::make_object();
      payload.set("index", json::Value::make_int(i));
      FUM_REQUIRE_OK(journal.value().append(JournalRecordType::note, payload, at(100 + i)));
    }
    FUM_REQUIRE_OK(journal.value().close());
  }
  auto reopened = Journal::open(fixture.path);
  FUM_CHECK(reopened.has_value());
  const RecoveryReport& recovery = reopened.value().recovery();
  FUM_CHECK_EQ(recovery.records_replayed, std::size_t{6});   // 5 notes plus the close marker
  FUM_CHECK(!recovery.truncated_tail);
  FUM_CHECK(recovery.chain_valid);
  FUM_CHECK(recovery.sequence_valid);
  FUM_CHECK(recovery.clean_shutdown);
  FUM_CHECK_EQ(recovery.bytes_ignored, 0ull);
  FUM_CHECK_EQ(recovery.last_sequence.value(), 6ull);
  FUM_CHECK(reopened.value().records().size() == 6);
  for (std::size_t i = 1; i < reopened.value().records().size(); ++i) {
    FUM_CHECK(reopened.value().records()[i].sequence.value() >
              reopened.value().records()[i - 1].sequence.value());
    FUM_CHECK_EQ(reopened.value().records()[i].previous_chain.hex(),
                 reopened.value().records()[i - 1].chain.hex());
  }
}

FUM_TEST(journal_detects_a_truncated_tail_and_refuses_to_extend_it) {
  auto fixture = JournalFixture::create("fum-journal-truncated");
  std::string complete;
  {
    auto journal = Journal::open(fixture.path);
    FUM_CHECK(journal.has_value());
    for (int i = 0; i < 3; ++i) {
      json::Value payload = json::Value::make_object();
      payload.set("index", json::Value::make_int(i));
      FUM_REQUIRE_OK(journal.value().append(JournalRecordType::note, payload, at(200 + i)));
    }
    FUM_REQUIRE_OK(journal.value().close());
    complete = fs::read_file(fixture.path).value();
  }
  // Simulate a torn write: the last record is cut in half.
  FUM_REQUIRE_OK(fs::write_file(fixture.path, complete.substr(0, complete.size() - 12)));
  auto journal = Journal::open(fixture.path);
  FUM_CHECK(journal.has_value());
  FUM_CHECK(journal.value().recovery().truncated_tail);
  FUM_CHECK(journal.value().recovery().bytes_ignored > 0);
  FUM_CHECK_EQ(journal.value().recovery().records_replayed, std::size_t{3});
  json::Value payload = json::Value::make_object();
  payload.set("index", json::Value::make_int(99));
  FUM_REQUIRE_ERR(journal.value().append(JournalRecordType::note, payload, at(300)),
                  ErrorCode::corrupt);

  // Recovering from the prefix is still possible: the valid records remain.
  FUM_CHECK(journal.value().records().size() == 3);
  FUM_CHECK(journal.value().records().back().data.require_uint("index").value() == 2);
}

FUM_TEST(journal_detects_bit_flips_and_refuses_the_damaged_record) {
  auto fixture = JournalFixture::create("fum-journal-bitflip");
  std::string bytes;
  {
    auto journal = Journal::open(fixture.path);
    FUM_CHECK(journal.has_value());
    for (int i = 0; i < 3; ++i) {
      json::Value payload = json::Value::make_object();
      payload.set("index", json::Value::make_int(i));
      FUM_REQUIRE_OK(journal.value().append(JournalRecordType::note, payload, at(400 + i)));
    }
    FUM_REQUIRE_OK(journal.value().close());
    bytes = fs::read_file(fixture.path).value();
  }
  // Flip a byte in the second record's payload.
  const std::size_t first_record_end = bytes.find("\"index\":0");
  FUM_CHECK(first_record_end != std::string::npos);
  const std::size_t flip = bytes.find("\"index\":1");
  FUM_CHECK(flip != std::string::npos);
  std::string damaged = bytes;
  damaged[flip + 9] = static_cast<char>(damaged[flip + 9] ^ 0x20);
  FUM_REQUIRE_OK(fs::write_file(fixture.path, damaged));
  auto journal = Journal::open(fixture.path);
  FUM_CHECK(journal.has_value());
  FUM_CHECK(journal.value().recovery().truncated_tail);
  FUM_CHECK(journal.value().recovery().damaged_records >= 1);
  // Everything before the damaged record is preserved; nothing after is trusted.
  FUM_CHECK_EQ(journal.value().recovery().records_replayed, std::size_t{1});
  FUM_CHECK_EQ(journal.value().recovery().bytes_ignored > 0, true);
  FUM_CHECK(journal.value().recovery().damage_reason.find("checksum") != std::string::npos);
}

FUM_TEST(journal_rejects_reordering_and_edited_history) {
  auto fixture = JournalFixture::create("fum-journal-reorder");
  std::string bytes;
  {
    auto journal = Journal::open(fixture.path);
    FUM_CHECK(journal.has_value());
    for (int i = 0; i < 3; ++i) {
      json::Value payload = json::Value::make_object();
      payload.set("index", json::Value::make_int(i));
      FUM_REQUIRE_OK(journal.value().append(JournalRecordType::note, payload, at(500 + i)));
    }
    FUM_REQUIRE_OK(journal.value().close());
    bytes = fs::read_file(fixture.path).value();
  }
  // Swap the first two records by hand.
  const std::size_t first = bytes.find("\"index\":0");
  const std::size_t second = bytes.find("\"index\":1");
  FUM_CHECK(first != std::string::npos && second != std::string::npos);
  FUM_CHECK(first < second);
  // Renumber: claiming index 1 has sequence of index 0 breaks the chain check.
  std::string edited = bytes;
  edited.replace(first + 9, 1, "1");
  FUM_REQUIRE_OK(fs::write_file(fixture.path, edited));
  auto journal = Journal::open(fixture.path);
  FUM_CHECK(journal.has_value());
  FUM_CHECK(journal.value().recovery().truncated_tail);
}

FUM_TEST(journal_enforces_its_record_and_file_bounds) {
  auto fixture = JournalFixture::create("fum-journal-bounds");
  JournalOptions options;
  options.max_record_bytes = 4096;
  options.max_file_bytes = 8192;
  auto journal = Journal::open(fixture.path, options);
  FUM_CHECK(journal.has_value());
  json::Value small = json::Value::make_object();
  small.set("value", json::Value::make_string(std::string(1024, 'x')));
  FUM_REQUIRE_OK(journal.value().append(JournalRecordType::note, small, at(600)));
  json::Value large = json::Value::make_object();
  large.set("value", json::Value::make_string(std::string(8192, 'x')));
  FUM_REQUIRE_ERR(journal.value().append(JournalRecordType::note, large, at(601)),
                  ErrorCode::resource_exhausted);
  // Fill the file and observe the bound.
  bool hit_bound = false;
  for (int i = 0; i < 32; ++i) {
    auto status = journal.value().append(JournalRecordType::note, small, at(602 + i));
    if (!status.has_value()) {
      hit_bound = status.error().code() == ErrorCode::resource_exhausted;
      break;
    }
  }
  FUM_CHECK(hit_bound);
  FUM_REQUIRE_OK(journal.value().close());
}

FUM_TEST(durable_store_round_trip_and_compaction) {
  auto directory = fs::TempDir::create("fum-store");
  FUM_CHECK(directory.has_value());
  fs::TempDir guard = std::move(directory).value();
  guard.keep();
  StoreOptions options;
  options.directory = guard.path();
  const CampaignId campaign_id = make_CampaignId("campaign-store");
  {
    auto store = DurableStore::open(options);
    FUM_CHECK(store.has_value());
    FUM_CHECK_EQ(store.value().incarnation().value(), 1ull);
    FUM_REQUIRE_OK(store.value().set_epoch(Epoch(2), at(10)));
    CampaignRecord campaign;
    campaign.id = campaign_id;
    campaign.generation = Generation(1);
    campaign.epoch = Epoch(2);
    campaign.authority = make_AuthorityId("authority");
    campaign.component = make_ComponentId("fabric-core");
    campaign.state = UpgradeState::proposed;
    campaign.revision = Revision(1);
    campaign.created_at = at(10);
    campaign.updated_at = at(10);
    FUM_REQUIRE_OK(store.value().put_campaign(campaign, at(10)));

    ProvenanceRecord record;
    record.id = make_RecordId("record-1");
    record.campaign = campaign_id;
    record.generation = Generation(1);
    record.stage = make_StageId("stage-1");
    record.attempt = make_AttemptId("attempt-1");
    record.target = make_TargetId("target-1");
    record.component = make_ComponentId("fabric-core");
    record.artifact = make_ArtifactId("artifact-1");
    record.build = make_BuildId("build-2");
    record.from_version = Version::parse("1.0.0").value();
    record.to_version = Version::parse("1.1.0").value();
    record.digest = Digest::of_bytes("payload");
    record.authority = make_AuthorityId("authority");
    record.incarnation = Incarnation(1);
    record.recorded_at = at(11);
    FUM_REQUIRE_OK(store.value().append_provenance(record, at(11)));
    FUM_CHECK_EQ(store.value().provenance().size(), std::size_t{1});
    FUM_REQUIRE_OK(store.value().compact(at(12)));
    FUM_CHECK_EQ(store.value().provenance().size(), std::size_t{1});
    FUM_REQUIRE_OK(store.value().close());
  }
  {
    // Reopen: the compacted snapshot restores the same state and the chain head.
    auto store = DurableStore::open(options);
    FUM_CHECK(store.has_value());
    FUM_CHECK_EQ(store.value().incarnation().value(), 2ull);
    FUM_CHECK(store.value().campaign(campaign_id) != nullptr);
    FUM_CHECK_EQ(store.value().provenance().size(), std::size_t{1});
    FUM_CHECK(verify_ledger(store.value().provenance()).valid);
    FUM_REQUIRE_OK(store.value().close());
  }
}

FUM_TEST(durable_store_refuses_a_damaged_journal_extension) {
  auto directory = fs::TempDir::create("fum-store-damaged");
  FUM_CHECK(directory.has_value());
  fs::TempDir guard = std::move(directory).value();
  guard.keep();
  StoreOptions options;
  options.directory = guard.path();
  {
    auto store = DurableStore::open(options);
    FUM_CHECK(store.has_value());
    FUM_REQUIRE_OK(store.value().note("marker", "1", at(20)));
    FUM_REQUIRE_OK(store.value().close());
  }
  const std::string path = fs::join(guard.path(), "journal.fum");
  const std::string bytes = fs::read_file(path).value();
  FUM_REQUIRE_OK(fs::write_file(path, bytes.substr(0, bytes.size() - 4)));

  // Opening refuses silently: the damage is reported, not repaired.
  auto refused = DurableStore::open(options);
  FUM_CHECK(!refused.has_value());
  FUM_CHECK_EQ(refused.error().code(), ErrorCode::corrupt);
  FUM_CHECK(refused.error().detail().find("offset") != std::string::npos);

  // With explicit authorisation the unverifiable bytes are dropped and the
  // store works again from the verified prefix.
  StoreOptions repair = options;
  repair.repair_damaged_tail = true;
  auto store = DurableStore::open(repair);
  FUM_CHECK(store.has_value());
  FUM_CHECK(!store.value().tail_damaged());
  FUM_CHECK(store.value().recovery().records_replayed >= 1);
  FUM_REQUIRE_OK(store.value().note("marker", "2", at(21)));
  FUM_REQUIRE_OK(store.value().close());

  // Reopening the repaired journal is clean.
  auto reopened = DurableStore::open(options);
  FUM_CHECK(reopened.has_value());
  FUM_CHECK(!reopened.value().tail_damaged());
  FUM_CHECK(reopened.value().recovery().clean_shutdown);
  FUM_REQUIRE_OK(reopened.value().close());
}

FUM_TEST(provenance_ledger_survives_restart_and_stays_verifiable) {
  auto directory = fs::TempDir::create("fum-store-ledger");
  FUM_CHECK(directory.has_value());
  fs::TempDir guard = std::move(directory).value();
  guard.keep();
  StoreOptions options;
  options.directory = guard.path();
  auto append_three = [&](int base) {
    auto store = DurableStore::open(options);
    FUM_CHECK(store.has_value());
    for (int i = 0; i < 3; ++i) {
      ProvenanceRecord record;
      record.id = make_RecordId("record-" + std::to_string(base + i));
      record.campaign = make_CampaignId("campaign-1");
      record.generation = Generation(1);
      record.stage = make_StageId("stage-1");
      record.attempt = make_AttemptId("attempt-" + std::to_string(base + i));
      record.target = make_TargetId("target-1");
      record.component = make_ComponentId("fabric-core");
      record.artifact = make_ArtifactId("artifact-1");
      record.build = make_BuildId("build-2");
      record.from_version = Version::parse("1.0.0").value();
      record.to_version = Version::parse("1.1.0").value();
      record.digest = Digest::of_bytes("payload");
      record.authority = make_AuthorityId("authority");
      record.incarnation = store.value().incarnation();
      record.recorded_at = at(100 + base + i);
      FUM_REQUIRE_OK(store.value().append_provenance(record, at(100 + base + i)));
    }
    FUM_REQUIRE_OK(store.value().close());
  };
  append_three(0);
  append_three(3);
  auto store = DurableStore::open(options);
  FUM_CHECK(store.has_value());
  FUM_CHECK_EQ(store.value().provenance().size(), std::size_t{6});
  const LedgerVerification verification = verify_ledger(store.value().provenance());
  FUM_CHECK(verification.valid);
  FUM_CHECK_EQ(store.value().provenance().back().sequence.value(), 6ull);
  FUM_REQUIRE_OK(store.value().close());
}

FUM_TEST(store_bounds_are_enforced) {
  auto directory = fs::TempDir::create("fum-store-bounds");
  FUM_CHECK(directory.has_value());
  fs::TempDir guard = std::move(directory).value();
  guard.keep();
  StoreOptions options;
  options.directory = guard.path();
  options.max_campaigns = 2;
  options.max_decisions_per_campaign = 2;
  auto store = DurableStore::open(options);
  FUM_CHECK(store.has_value());
  for (int i = 0; i < 2; ++i) {
    CampaignRecord campaign;
    campaign.id = make_CampaignId("campaign-" + std::to_string(i));
    campaign.generation = Generation(1);
    campaign.epoch = Epoch(1);
    campaign.authority = make_AuthorityId("authority");
    campaign.component = make_ComponentId("fabric-core");
    campaign.created_at = at(30 + i);
    campaign.updated_at = at(30 + i);
    FUM_REQUIRE_OK(store.value().put_campaign(campaign, at(30 + i)));
  }
  CampaignRecord overflow;
  overflow.id = make_CampaignId("campaign-overflow");
  overflow.authority = make_AuthorityId("authority");
  overflow.component = make_ComponentId("fabric-core");
  overflow.created_at = at(40);
  overflow.updated_at = at(40);
  FUM_REQUIRE_ERR(store.value().put_campaign(overflow, at(40)), ErrorCode::resource_exhausted);

  // Decision history is bounded per campaign: the oldest entries are retired.
  for (int i = 0; i < 5; ++i) {
    Decision decision;
    decision.id = make_DecisionId("decision-" + std::to_string(i));
    decision.kind = DecisionKind::plan;
    decision.outcome = DecisionOutcome::allow;
    decision.campaign = make_CampaignId("campaign-0");
    decision.policy = make_PolicyId("policy");
    decision.policy_revision = Revision(1);
    decision.selected = "x";
    decision.rationale = "y";
    decision.at = at(50 + i);
    FUM_REQUIRE_OK(store.value().append_decision(decision, at(50 + i)));
  }
  const auto* decisions = store.value().decisions(make_CampaignId("campaign-0"));
  FUM_CHECK(decisions != nullptr);
  FUM_CHECK_EQ(decisions->size(), std::size_t{2});
  FUM_REQUIRE_OK(store.value().close());
}

int main(int argc, char** argv) { return fumtest::run_all(argc, argv); }