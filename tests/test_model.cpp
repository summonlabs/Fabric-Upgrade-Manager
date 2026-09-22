#include "test_support.hpp"

#include "fum/model/campaign.hpp"
#include "fum/model/decision.hpp"
#include "fum/model/evidence.hpp"
#include "fum/model/lifecycle.hpp"
#include "fum/model/policy.hpp"
#include "fum/model/provenance.hpp"
#include "fum/model/skew.hpp"

using namespace fum;

namespace {

TransitionGuards fully_open_guards() {
  TransitionGuards guards;
  guards.preflight_current = true;
  guards.artifact_integrity_verified = true;
  guards.rollback_plan_recorded = true;
  guards.irreversible_acknowledged = true;
  guards.adapter_supports_strategy = true;
  guards.staging_complete = true;
  guards.activation_complete = true;
  guards.verification_fresh_passed = true;
  guards.skew_within_budget = true;
  guards.drain_satisfied = true;
  guards.all_stages_complete = true;
  guards.failure_recorded = true;
  guards.rollback_eligible = true;
  guards.rollback_complete = true;
  guards.manual_release_required = true;
  return guards;
}

}  // namespace

FUM_TEST(lifecycle_happy_path_is_permitted) {
  const TransitionGuards guards = fully_open_guards();
  const UpgradeState path[] = {UpgradeState::validated, UpgradeState::prepared,
                               UpgradeState::staged, UpgradeState::activating,
                               UpgradeState::verifying, UpgradeState::completed};
  UpgradeState current = UpgradeState::proposed;
  for (const auto next : path) {
    const TransitionVerdict verdict = evaluate_transition(current, next, guards);
    FUM_CHECK(verdict.allowed);
    current = next;
  }
  FUM_CHECK(upgrade_state_is_terminal(current));
}

FUM_TEST(lifecycle_refuses_transitions_without_guards) {
  TransitionGuards guards;
  auto verdict = evaluate_transition(UpgradeState::proposed, UpgradeState::validated, guards);
  FUM_CHECK(!verdict.allowed);
  FUM_CHECK(verdict.unmet.size() == 2);
  FUM_CHECK(verdict.explain().find("preflight-current") != std::string::npos);

  guards.preflight_current = true;
  guards.artifact_integrity_verified = true;
  FUM_CHECK(evaluate_transition(UpgradeState::proposed, UpgradeState::validated, guards).allowed);

  // Preparing without a recorded rollback plan is refused.
  TransitionGuards prepare_guards;
  prepare_guards.irreversible_acknowledged = true;
  verdict = evaluate_transition(UpgradeState::validated, UpgradeState::prepared, prepare_guards);
  FUM_CHECK(!verdict.allowed);
  FUM_CHECK(verdict.explain().find("rollback-plan-recorded") != std::string::npos);

  // Activation without the skew budget is refused.
  TransitionGuards staged_guards;
  staged_guards.drain_satisfied = true;
  verdict = evaluate_transition(UpgradeState::staged, UpgradeState::activating, staged_guards);
  FUM_CHECK(!verdict.allowed);
  FUM_CHECK(verdict.explain().find("skew-within-budget") != std::string::npos);

  // Completion without fresh verification is refused.
  TransitionGuards verifying_guards;
  verifying_guards.all_stages_complete = true;
  verdict = evaluate_transition(UpgradeState::verifying, UpgradeState::completed, verifying_guards);
  FUM_CHECK(!verdict.allowed);
  FUM_CHECK(verdict.explain().find("verification-fresh-passed") != std::string::npos);
}

FUM_TEST(lifecycle_terminal_states_are_final) {
  const TransitionGuards guards = fully_open_guards();
  for (const auto terminal : {UpgradeState::completed, UpgradeState::rolled_back}) {
    FUM_CHECK(upgrade_state_is_terminal(terminal));
    for (std::size_t i = 0; i < kUpgradeStateCount; ++i) {
      const auto target = static_cast<UpgradeState>(i);
      const TransitionVerdict verdict = evaluate_transition(terminal, target, guards);
      if (target == terminal) {
        continue;
      }
      FUM_CHECK(!verdict.allowed);
    }
  }
  FUM_CHECK(!upgrade_state_is_terminal(UpgradeState::failed));
  FUM_CHECK(upgrade_state_permits_pause(UpgradeState::staged));
  FUM_CHECK(!upgrade_state_permits_pause(UpgradeState::activating));
  FUM_CHECK(upgrade_state_is_active(UpgradeState::verifying));
}

FUM_TEST(lifecycle_rollback_chain) {
  TransitionGuards guards;
  guards.failure_recorded = true;
  FUM_CHECK(evaluate_transition(UpgradeState::activating, UpgradeState::failed, guards).allowed);
  guards.rollback_eligible = true;
  FUM_CHECK(evaluate_transition(UpgradeState::failed, UpgradeState::rollback_planned, guards).allowed);
  FUM_CHECK(!evaluate_transition(UpgradeState::failed, UpgradeState::rolling_back, guards).allowed);
  FUM_CHECK(evaluate_transition(UpgradeState::rollback_planned, UpgradeState::rolling_back, guards).allowed);
  guards.rollback_complete = true;
  FUM_CHECK(evaluate_transition(UpgradeState::rolling_back, UpgradeState::rolled_back, guards).allowed);
  FUM_CHECK(!evaluate_transition(UpgradeState::rolled_back, UpgradeState::staged, guards).allowed);
}

FUM_TEST(evidence_freshness_requires_the_current_incarnation) {
  const Timestamp now = Timestamp::from_unix_nanos(1000 * kNanosPerSecond);
  Evidence<int> evidence(42, now, Duration::from_seconds(30), Incarnation(7),
                         EvidenceSource::health_probe, "probe");
  FUM_CHECK(evidence.is_fresh(now.plus(Duration::from_seconds(5)), Incarnation(7)));
  // A different incarnation is stale even at zero age.
  FUM_CHECK(!evidence.is_fresh(now.plus(Duration::from_seconds(5)), Incarnation(8)));
  FUM_CHECK(evidence.freshness(now.plus(Duration::from_seconds(5)), Incarnation(8))
                .reason.find("incarnation") != std::string::npos);
  // Expired evidence is stale.
  FUM_CHECK(!evidence.is_fresh(now.plus(Duration::from_seconds(31)), Incarnation(7)));
  // Evidence stamped in the future is stale.
  FUM_CHECK(!evidence.is_fresh(now.plus(Duration::from_seconds(-1)), Incarnation(7)));
  // Zero validity is never fresh.
  Evidence<int> zero(1, now, Duration{}, Incarnation(7), EvidenceSource::adapter_observation, "x");
  FUM_CHECK(!zero.is_fresh(now, Incarnation(7)));
  // Absent evidence is never fresh.
  Evidence<int> absent;
  FUM_CHECK(!absent.is_fresh(now, Incarnation(7)));
  FUM_CHECK(!absent.present());
}

FUM_TEST(skew_budget_assessment) {
  SkewBudget budget;
  budget.max_major_skew = 0;
  budget.max_minor_skew = 1;
  budget.max_patch_skew = 4;
  std::vector<std::pair<std::string, Version>> live = {
      {"target-b", Version::parse("1.0.0").value()},
      {"target-a", Version::parse("1.1.0").value()},
  };
  const Version newest = Version::parse("1.1.0").value();
  auto assessment = assess_skew(budget, live, newest);
  FUM_CHECK(assessment.within_budget);
  FUM_CHECK_EQ(assessment.worst_distance, 1000u);

  // Two minor releases behind exceeds a one-minor budget.
  live[0].second = Version::parse("0.9.0").value();
  assessment = assess_skew(budget, live, newest);
  FUM_CHECK(!assessment.within_budget);
  FUM_CHECK(assessment.violations.front().detail.find("major skew") != std::string::npos);

  const Version minor_newest = Version::parse("1.4.0").value();
  std::vector<std::pair<std::string, Version>> behind = {
      {"target-a", Version::parse("1.2.0").value()}};
  assessment = assess_skew(budget, behind, minor_newest);
  FUM_CHECK(!assessment.within_budget);
  FUM_CHECK(assessment.violations.front().detail.find("minor skew") != std::string::npos);
  SkewBudget two_minor = budget;
  two_minor.max_minor_skew = 2;
  FUM_CHECK(assess_skew(two_minor, behind, minor_newest).within_budget);

  const Version patch_newest = Version::parse("1.4.9").value();
  std::vector<std::pair<std::string, Version>> patch_behind = {
      {"target-a", Version::parse("1.4.0").value()}};
  assessment = assess_skew(budget, patch_behind, patch_newest);
  FUM_CHECK(!assessment.within_budget);
  FUM_CHECK(assessment.violations.front().detail.find("patch skew") != std::string::npos);

  // Deterministic ordering by subject and one violation per target.
  live = {{"target-c", Version::parse("1.4.0").value()},
          {"target-b", Version::parse("1.4.0").value()}};
  assessment = assess_skew(budget, live, newest);
  FUM_CHECK_EQ(assessment.violations.size(), std::size_t{2});
  FUM_CHECK_EQ(assessment.violations.front().subject, std::string("target-b"));

  // Explicit waivers are honoured.
  budget.allowed_pairs.emplace_back(Version::parse("1.4.0").value(), newest);
  FUM_CHECK(assess_skew(budget, live, newest).within_budget);

  // A target ahead of the campaign artifact is a violation.
  std::vector<std::pair<std::string, Version>> ahead = {{"target-a", Version::parse("2.0.0").value()}};
  SkewBudget generous;
  generous.max_major_skew = 8;
  FUM_CHECK(!assess_skew(generous, ahead, newest).within_budget);
  FUM_CHECK(assess_skew(generous, ahead, newest).violations.front().detail.find("newer") !=
            std::string::npos);
  // Cross-major skew is refused unless the budget allows it.
  std::vector<std::pair<std::string, Version>> cross = {{"target-a", Version::parse("2.0.0").value()}};
  SkewBudget mixed = generous;
  mixed.max_major_skew = 0;
  FUM_CHECK(!assess_skew(mixed, cross, newest).within_budget);
  // An unknown version is a violation.
  std::vector<std::pair<std::string, Version>> unknown = {{"target-a", Version{}}};
  FUM_CHECK(!assess_skew(generous, unknown, newest).within_budget);
  // JSON round trip.
  auto parsed = SkewBudget::from_json(budget.to_json());
  FUM_CHECK(parsed.has_value());
  FUM_CHECK(parsed.value().to_json() == budget.to_json());
}

FUM_TEST(provenance_ledger_chain_detects_tampering) {
  std::vector<ProvenanceRecord> records;
  Digest previous;
  Sequence sequence(0);
  for (int i = 0; i < 4; ++i) {
    ProvenanceRecord record;
    sequence = sequence.try_next().value();
    record.sequence = sequence;
    record.id = make_RecordId("record-" + std::to_string(i));
    record.campaign = make_CampaignId("campaign-1");
    record.generation = Generation(1);
    record.stage = make_StageId("stage-1");
    record.attempt = make_AttemptId("attempt-" + std::to_string(i));
    record.target = make_TargetId("target-" + std::to_string(i));
    record.component = make_ComponentId("fabric-core");
    record.artifact = make_ArtifactId("artifact-1");
    record.build = make_BuildId("build-2");
    record.from_version = Version::parse("1.0.0").value();
    record.to_version = Version::parse("1.1.0").value();
    record.digest = Digest::of_bytes("payload");
    record.authority = make_AuthorityId("authority");
    record.incarnation = Incarnation(3);
    record.recorded_at = Timestamp::from_unix_nanos(1000 + i);
    record.previous = previous;
    record.chain = record.compute_chain(previous);
    previous = record.chain;
    records.push_back(record);
  }
  FUM_CHECK(verify_ledger(records).valid);
  auto tampered = records;
  tampered[2].to_version = Version::parse("9.9.9").value();
  auto verification = verify_ledger(tampered);
  FUM_CHECK(!verification.valid);
  FUM_CHECK_EQ(verification.first_bad_index, std::size_t{2});
  auto reordered = records;
  std::swap(reordered[0], reordered[1]);
  FUM_CHECK(!verify_ledger(reordered).valid);
  // JSON round trip preserves the chain.
  auto parsed = ProvenanceRecord::from_json(records[1].to_json());
  FUM_CHECK(parsed.has_value());
  FUM_CHECK_EQ(parsed.value().chain.hex(), records[1].chain.hex());
  FUM_CHECK(parsed.value().to_json() == records[1].to_json());
}

FUM_TEST(decision_fingerprints_are_deterministic) {
  Decision decision;
  decision.id = make_DecisionId("dec-1");
  decision.kind = DecisionKind::preflight;
  decision.outcome = DecisionOutcome::deny;
  decision.campaign = make_CampaignId("campaign-1");
  decision.generation = Generation(2);
  decision.authority = make_AuthorityId("authority");
  decision.epoch = Epoch(4);
  decision.policy = make_PolicyId("policy");
  decision.policy_revision = Revision(3);
  decision.inputs = {DecisionInput{"blockers", "1"}};
  decision.selected = "refuse execution";
  decision.rationale = "the registry declares the upgrade incompatible";
  decision.rejected = {RejectedAlternative{"activate", "incompatible"}};
  decision.at = Timestamp::from_unix_nanos(12345);
  const std::string fingerprint = decision.fingerprint();

  Decision copy = decision;
  copy.id = make_DecisionId("dec-999");
  copy.at = Timestamp::from_unix_nanos(999999);
  copy.incarnation = Incarnation(77);
  FUM_CHECK_EQ(copy.fingerprint(), fingerprint);

  Decision changed = decision;
  changed.rationale = "something else";
  FUM_CHECK_NE(changed.fingerprint(), fingerprint);
  changed = decision;
  changed.selected = "proceed";
  FUM_CHECK_NE(changed.fingerprint(), fingerprint);

  auto parsed = Decision::from_json(decision.to_json());
  FUM_CHECK(parsed.has_value());
  FUM_CHECK_EQ(parsed.value().fingerprint(), fingerprint);
  FUM_CHECK(parsed.value().explain().find("fingerprint") != std::string::npos);
}

FUM_TEST(artifact_descriptor_round_trip_and_validation) {
  ArtifactDescriptor artifact;
  artifact.set_id(make_ArtifactId("artifact-1"));
  artifact.set_component(make_ComponentId("fabric-core"));
  artifact.set_version(Version::parse("1.2.3").value());
  artifact.set_build(make_BuildId("build-9"));
  artifact.set_component_type(ComponentType::firmware);
  artifact.set_digest(Digest::of_bytes("payload"));
  artifact.set_size_bytes(7);
  artifact.set_platforms({"linux/x86_64"});
  FUM_CHECK(!artifact.validate().has_value());   // irreversible without a declared boundary
  RollbackCompatibility rollback;
  rollback.reversible = true;
  artifact.set_rollback(rollback);
  FUM_CHECK(artifact.validate().has_value());
  auto parsed = ArtifactDescriptor::from_json(artifact.to_json());
  FUM_CHECK(parsed.has_value());
  FUM_CHECK(parsed.value().to_json() == artifact.to_json());
  FUM_CHECK(parsed.value().supported_on("linux/x86_64"));
  FUM_CHECK(!parsed.value().supported_on("windows/x86_64"));

  // Malformed descriptors are refused.
  json::Value broken = artifact.to_json();
  broken.set("size_bytes", json::Value::make_uint(0));
  FUM_CHECK(!ArtifactDescriptor::from_json(broken).has_value());
  broken = artifact.to_json();
  broken.erase("digest");
  FUM_CHECK(!ArtifactDescriptor::from_json(broken).has_value());
  broken = artifact.to_json();
  broken.set("platforms", json::Value::make_array());
  FUM_CHECK(!ArtifactDescriptor::from_json(broken).has_value());
  broken = artifact.to_json();
  broken.set("rollback", [] {
    json::Value value = json::Value::make_object();
    value.set("reversible", json::Value::make_bool(false));
    value.set("irreversible_steps", json::Value::make_array());
    return value;
  }());
  FUM_CHECK(!ArtifactDescriptor::from_json(broken).has_value());   // irreversible without boundary
}

FUM_TEST(integrity_assessment_gates_artifact_use) {
  const std::string payload = "artifact-bytes";
  ArtifactDescriptor artifact;
  artifact.set_id(make_ArtifactId("artifact-1"));
  artifact.set_component(make_ComponentId("fabric-core"));
  artifact.set_version(Version::parse("1.0.0").value());
  artifact.set_build(make_BuildId("build-1"));
  artifact.set_digest(Digest::of_bytes(payload));
  artifact.set_size_bytes(static_cast<std::uint64_t>(payload.size()));
  artifact.set_platforms({"any/any"});
  RollbackCompatibility rollback;
  rollback.reversible = true;
  artifact.set_rollback(rollback);
  auto assessment = verify_artifact_bytes(artifact, payload);
  FUM_CHECK(assessment.verified);
  assessment = verify_artifact_bytes(artifact, payload + "x");
  FUM_CHECK(!assessment.verified);
  FUM_CHECK(assessment.detail.find("mismatch") != std::string::npos);
  // Size mismatch with a matching digest of the declared bytes.
  ArtifactDescriptor resized = artifact;
  resized.set_size_bytes(1);
  FUM_CHECK(!verify_artifact_bytes(resized, payload).verified);
}

FUM_TEST(adapter_claims_must_be_self_consistent) {
  AdapterClaims claims;
  FUM_CHECK(!claims.validate().has_value());   // no strategy at all
  claims.strategies = {StrategyKind::in_place, StrategyKind::in_place};
  FUM_CHECK(!claims.validate().has_value());   // duplicate
  claims.strategies = {StrategyKind::control_plane_generation_handoff};
  FUM_CHECK(!claims.validate().has_value());   // handoff without the capability
  claims.supports_generation_handoff = true;
  FUM_CHECK(claims.validate().has_value());
  claims.max_parallel_activations = 0;
  FUM_CHECK(!claims.validate().has_value());
  claims.max_parallel_activations = 1024;
  FUM_CHECK(!claims.validate().has_value());
  claims.max_parallel_activations = 2;
  FUM_CHECK(claims.validate().has_value());
  claims.strategies = {StrategyKind::redundant_pair_rolling};
  FUM_CHECK(!claims.validate().has_value());   // rolling requires a health probe
  claims.supports_health_probe = true;
  FUM_CHECK(claims.validate().has_value());
  auto parsed = AdapterClaims::from_json(claims.to_json());
  FUM_CHECK(parsed.has_value());
  FUM_CHECK_EQ(parsed.value().max_parallel_activations, 2u);
}

FUM_TEST(policy_round_trip_and_bounds) {
  Policy policy;
  policy.id = make_PolicyId("policy-1");
  policy.revision = Revision(2);
  FUM_CHECK(policy.validate().has_value());
  policy.max_concurrent_activations = 0;
  FUM_CHECK(!policy.validate().has_value());
  policy.max_concurrent_activations = 4;
  policy.health_validity = Duration{};
  FUM_CHECK(!policy.validate().has_value());
  policy.health_validity = Duration::from_seconds(30);
  auto parsed = Policy::from_json(policy.to_json());
  FUM_CHECK(parsed.has_value());
  FUM_CHECK(parsed.value().to_json() == policy.to_json());
  FUM_CHECK_EQ(parsed.value().fingerprint(), policy.fingerprint());
  json::Value broken = policy.to_json();
  broken.set("max_attempts_per_target", json::Value::make_uint(0));
  FUM_CHECK(!Policy::from_json(broken).has_value());
}

FUM_TEST(campaign_and_attempt_records_round_trip) {
  CampaignRecord campaign;
  campaign.id = make_CampaignId("campaign-1");
  campaign.generation = Generation(3);
  campaign.epoch = Epoch(2);
  campaign.authority = make_AuthorityId("authority");
  campaign.component = make_ComponentId("fabric-core");
  campaign.state = UpgradeState::staged;
  campaign.strategy = StrategyKind::restart_based;
  campaign.revision = Revision(5);
  campaign.owner_incarnation = Incarnation(9);
  campaign.created_at = Timestamp::from_unix_nanos(1000);
  campaign.updated_at = Timestamp::from_unix_nanos(2000);
  campaign.irreversible_acknowledged = true;
  campaign.acknowledged_steps = {make_StepId("activate")};
  campaign.rollback_plan = "restore the previous build";
  campaign.rollback_eligible = true;
  ArtifactDescriptor artifact;
  artifact.set_id(make_ArtifactId("artifact-1"));
  artifact.set_component(make_ComponentId("fabric-core"));
  artifact.set_version(Version::parse("1.1.0").value());
  artifact.set_build(make_BuildId("build-2"));
  artifact.set_digest(Digest::of_bytes("payload"));
  artifact.set_size_bytes(7);
  artifact.set_platforms({"linux/x86_64"});
  RollbackCompatibility rollback;
  rollback.reversible = true;
  artifact.set_rollback(rollback);
  campaign.artifact = artifact;
  StageProgress progress;
  progress.id = make_StageId("stage-1");
  progress.state = StageState::prepared;
  progress.started_at = Timestamp::from_unix_nanos(1500);
  campaign.stages.push_back(progress);
  auto parsed = CampaignRecord::from_json(campaign.to_json());
  FUM_CHECK(parsed.has_value());
  FUM_CHECK(parsed.value().to_json() == campaign.to_json());
  FUM_CHECK(parsed.value().state == UpgradeState::staged);

  AttemptRecord attempt;
  attempt.id = make_AttemptId("attempt-1");
  attempt.campaign = campaign.id;
  attempt.generation = Generation(3);
  attempt.stage = progress.id;
  attempt.target = make_TargetId("target-1");
  attempt.artifact = make_ArtifactId("artifact-1");
  attempt.build = make_BuildId("build-2");
  attempt.from_version = Version::parse("1.0.0").value();
  attempt.to_version = Version::parse("1.1.0").value();
  attempt.state = AttemptState::verified;
  attempt.owner = Incarnation(9);
  attempt.authority = make_AuthorityId("authority");
  attempt.revision = Revision(4);
  attempt.operation_index = Sequence(2);
  attempt.evidence_at = Timestamp::from_unix_nanos(3000);
  attempt.evidence_validity = Duration::from_seconds(60);
  attempt.observed_version = Version::parse("1.1.0").value();
  attempt.observed_build = make_BuildId("build-2");
  attempt.started_at = Timestamp::from_unix_nanos(2500);
  attempt.updated_at = Timestamp::from_unix_nanos(3000);
  auto attempt_parsed = AttemptRecord::from_json(attempt.to_json());
  FUM_CHECK(attempt_parsed.has_value());
  FUM_CHECK(attempt_parsed.value().to_json() == attempt.to_json());
  FUM_CHECK(attempt_parsed.value().evidence_validity == Duration::from_seconds(60));

  json::Value broken = campaign.to_json();
  broken.set("state", json::Value::make_string("teleported"));
  FUM_CHECK(!CampaignRecord::from_json(broken).has_value());
}

int main(int argc, char** argv) { return fumtest::run_all(argc, argv); }
