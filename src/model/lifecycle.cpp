#include "fum/model/lifecycle.hpp"

#include <array>

namespace fum {
namespace {

struct Rule {
  UpgradeState from;
  UpgradeState to;
  // Guard expression: every listed guard must be true.
  std::array<bool TransitionGuards::*, 4> requirements;
  std::size_t requirement_count;
  const char* description;
};

constexpr std::array<bool TransitionGuards::*, 4> kNone = {nullptr, nullptr, nullptr, nullptr};

constexpr Rule kRules[] = {
    {UpgradeState::proposed, UpgradeState::validated,
     {&TransitionGuards::preflight_current, &TransitionGuards::artifact_integrity_verified, nullptr,
      nullptr},
     2, "preflight must be current and artifact integrity verified"},
    {UpgradeState::blocked, UpgradeState::validated,
     {&TransitionGuards::preflight_current, &TransitionGuards::artifact_integrity_verified, nullptr,
      nullptr},
     2, "a blocked campaign may return to validated once preflight is current again"},
    {UpgradeState::paused, UpgradeState::validated,
     {&TransitionGuards::preflight_current, &TransitionGuards::artifact_integrity_verified, nullptr,
      nullptr},
     2, "a paused campaign resumes at validated when preflight is still current"},
    {UpgradeState::validated, UpgradeState::prepared,
     {&TransitionGuards::rollback_plan_recorded, &TransitionGuards::irreversible_acknowledged,
      nullptr, nullptr},
     2, "prepare requires a recorded rollback plan and explicit acknowledgement of irreversible "
        "steps"},
    {UpgradeState::prepared, UpgradeState::staged,
     {&TransitionGuards::adapter_supports_strategy, &TransitionGuards::staging_complete, nullptr,
      nullptr},
     2, "staging requires adapter support for the selected strategy and completed preparation on "
        "every target"},
    {UpgradeState::staged, UpgradeState::activating,
     {&TransitionGuards::skew_within_budget, &TransitionGuards::drain_satisfied, nullptr, nullptr}, 2,
     "activation requires the version skew budget and drain obligations to hold"},
    {UpgradeState::activating, UpgradeState::verifying, {&TransitionGuards::activation_complete,
                                                         nullptr, nullptr, nullptr},
     1, "verification begins only after every target in the stage reports the new version"},
    {UpgradeState::verifying, UpgradeState::staged,
     {&TransitionGuards::skew_within_budget, &TransitionGuards::drain_satisfied, nullptr, nullptr}, 2,
     "the next stage begins from staged once verification passes"},
    {UpgradeState::verifying, UpgradeState::completed,
     {&TransitionGuards::verification_fresh_passed, &TransitionGuards::all_stages_complete, nullptr,
      nullptr},
     2, "completion requires fresh passing verification for every stage"},
    {UpgradeState::verifying, UpgradeState::failed, {&TransitionGuards::failure_recorded, nullptr,
                                                     nullptr, nullptr},
     1, "a failed verification records a failure"},
    {UpgradeState::activating, UpgradeState::failed, {&TransitionGuards::failure_recorded, nullptr,
                                                      nullptr, nullptr},
     1, "a failed activation records a failure"},
    {UpgradeState::staged, UpgradeState::failed, {&TransitionGuards::failure_recorded, nullptr,
                                                  nullptr, nullptr},
     1, "a failed preparation records a failure"},
    {UpgradeState::prepared, UpgradeState::failed, {&TransitionGuards::failure_recorded, nullptr,
                                                    nullptr, nullptr},
     1, "an aborted prepared campaign records a failure"},
    {UpgradeState::paused, UpgradeState::failed, {&TransitionGuards::failure_recorded, nullptr,
                                                  nullptr, nullptr},
     1, "an aborted paused campaign records a failure"},
    {UpgradeState::validated, UpgradeState::failed, {&TransitionGuards::failure_recorded, nullptr,
                                                     nullptr, nullptr},
     1, "an aborted validated campaign records a failure"},
    {UpgradeState::failed, UpgradeState::rollback_planned,
     {&TransitionGuards::rollback_eligible, nullptr, nullptr, nullptr},
     1, "rollback must be eligible and explicitly planned"},
    {UpgradeState::rollback_planned, UpgradeState::rolling_back, {&TransitionGuards::rollback_eligible,
                                                                  nullptr, nullptr, nullptr},
     1, "rollback execution requires rollback eligibility"},
    {UpgradeState::rolling_back, UpgradeState::rolled_back, {&TransitionGuards::rollback_complete,
                                                             nullptr, nullptr, nullptr},
     1, "rolled back requires every rollback step to complete"},
    {UpgradeState::rolling_back, UpgradeState::failed, {&TransitionGuards::failure_recorded, nullptr,
                                                        nullptr, nullptr},
     1, "a failed rollback records a failure"},
    // Operator controls.
    {UpgradeState::validated, UpgradeState::paused, {&TransitionGuards::manual_release_required,
                                                     nullptr, nullptr, nullptr},
     0, "a validated campaign can be paused before execution"},
    {UpgradeState::prepared, UpgradeState::paused, {&TransitionGuards::manual_release_required,
                                                    nullptr, nullptr, nullptr},
     0, "a prepared campaign can be paused before execution"},
    {UpgradeState::staged, UpgradeState::paused, {&TransitionGuards::manual_release_required, nullptr,
                                                  nullptr, nullptr},
     0, "a staged campaign can be paused between stages"},
    {UpgradeState::paused, UpgradeState::staged, {&TransitionGuards::staging_complete, nullptr,
                                                  nullptr, nullptr},
     1, "resuming a staged campaign requires staging to still be complete"},
    {UpgradeState::blocked, UpgradeState::paused, {&TransitionGuards::manual_release_required,
                                                   nullptr, nullptr, nullptr},
     0, "a blocked campaign can be parked by an operator"},
    {UpgradeState::blocked, UpgradeState::failed, {&TransitionGuards::failure_recorded, nullptr,
                                                   nullptr, nullptr},
     1, "a blocked campaign that cannot proceed is failed so rollback can be planned"},
    // Every non-terminal state can be blocked by policy or a failed gate.
    {UpgradeState::proposed, UpgradeState::blocked, {&TransitionGuards::failure_recorded, nullptr,
                                                     nullptr, nullptr},
     1, "a proposed campaign can be blocked when preflight cannot proceed"},
    {UpgradeState::validated, UpgradeState::blocked, {&TransitionGuards::failure_recorded, nullptr,
                                                      nullptr, nullptr},
     1, "a validated campaign can be blocked when a gate fails"},
    {UpgradeState::prepared, UpgradeState::blocked, {&TransitionGuards::failure_recorded, nullptr,
                                                     nullptr, nullptr},
     1, "a prepared campaign can be blocked when a gate fails"},
    {UpgradeState::staged, UpgradeState::blocked, {&TransitionGuards::failure_recorded, nullptr,
                                                   nullptr, nullptr},
     1, "a staged campaign can be blocked when a gate fails"},
    {UpgradeState::activating, UpgradeState::blocked, {&TransitionGuards::failure_recorded, nullptr,
                                                       nullptr, nullptr},
     1, "an activating campaign can be blocked when a gate fails"},
    {UpgradeState::verifying, UpgradeState::blocked, {&TransitionGuards::failure_recorded, nullptr,
                                                      nullptr, nullptr},
     1, "a verifying campaign can be blocked when a gate fails"},
    {UpgradeState::paused, UpgradeState::paused, kNone, 0, "pause is idempotent"},
    {UpgradeState::blocked, UpgradeState::blocked, kNone, 0, "block is idempotent"},
};

bool guard_value(const TransitionGuards& guards, bool TransitionGuards::*member) {
  return member == nullptr ? true : guards.*member;
}

const char* describe_guard(bool TransitionGuards::*member) {
  if (member == &TransitionGuards::preflight_current) return "preflight-current";
  if (member == &TransitionGuards::artifact_integrity_verified) return "artifact-integrity-verified";
  if (member == &TransitionGuards::rollback_plan_recorded) return "rollback-plan-recorded";
  if (member == &TransitionGuards::irreversible_acknowledged) return "irreversible-acknowledged";
  if (member == &TransitionGuards::adapter_supports_strategy) return "adapter-supports-strategy";
  if (member == &TransitionGuards::staging_complete) return "staging-complete";
  if (member == &TransitionGuards::activation_complete) return "activation-complete";
  if (member == &TransitionGuards::verification_fresh_passed) return "verification-fresh-passed";
  if (member == &TransitionGuards::skew_within_budget) return "skew-within-budget";
  if (member == &TransitionGuards::drain_satisfied) return "drain-satisfied";
  if (member == &TransitionGuards::all_stages_complete) return "all-stages-complete";
  if (member == &TransitionGuards::failure_recorded) return "failure-recorded";
  if (member == &TransitionGuards::rollback_eligible) return "rollback-eligible";
  if (member == &TransitionGuards::rollback_complete) return "rollback-complete";
  if (member == &TransitionGuards::manual_release_required) return "manual-release-required";
  return "unknown-guard";
}

}  // namespace

const char* upgrade_state_name(UpgradeState state) noexcept {
  switch (state) {
    case UpgradeState::proposed: return "proposed";
    case UpgradeState::validated: return "validated";
    case UpgradeState::prepared: return "prepared";
    case UpgradeState::staged: return "staged";
    case UpgradeState::activating: return "activating";
    case UpgradeState::verifying: return "verifying";
    case UpgradeState::completed: return "completed";
    case UpgradeState::blocked: return "blocked";
    case UpgradeState::paused: return "paused";
    case UpgradeState::failed: return "failed";
    case UpgradeState::rollback_planned: return "rollback-planned";
    case UpgradeState::rolling_back: return "rolling-back";
    case UpgradeState::rolled_back: return "rolled-back";
  }
  return "unknown";
}

Result<UpgradeState> parse_upgrade_state(std::string_view text) {
  for (std::size_t i = 0; i < kUpgradeStateCount; ++i) {
    const auto state = static_cast<UpgradeState>(i);
    if (text == upgrade_state_name(state)) {
      return state;
    }
  }
  return make_error(ErrorCode::invalid_argument, "unknown upgrade state", std::string(text));
}

bool upgrade_state_is_terminal(UpgradeState state) noexcept {
  return state == UpgradeState::completed || state == UpgradeState::rolled_back;
}

bool upgrade_state_is_active(UpgradeState state) noexcept {
  return state == UpgradeState::activating || state == UpgradeState::verifying ||
         state == UpgradeState::rolling_back;
}

bool upgrade_state_permits_pause(UpgradeState state) noexcept {
  return state == UpgradeState::validated || state == UpgradeState::prepared ||
         state == UpgradeState::staged || state == UpgradeState::paused ||
         state == UpgradeState::blocked;
}

bool upgrade_state_is_pre_activation(UpgradeState state) noexcept {
  return state == UpgradeState::proposed || state == UpgradeState::validated ||
         state == UpgradeState::prepared || state == UpgradeState::staged ||
         state == UpgradeState::paused || state == UpgradeState::blocked;
}

const char* attempt_state_name(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::created: return "created";
    case AttemptState::preparing: return "preparing";
    case AttemptState::prepared: return "prepared";
    case AttemptState::activating: return "activating";
    case AttemptState::activated: return "activated";
    case AttemptState::verifying: return "verifying";
    case AttemptState::verified: return "verified";
    case AttemptState::failed: return "failed";
    case AttemptState::aborted: return "aborted";
    case AttemptState::rolled_back: return "rolled-back";
    case AttemptState::abandoned: return "abandoned";
    case AttemptState::rolling_back: return "rolling-back";
  }
  return "unknown";
}

bool attempt_state_is_terminal(AttemptState state) noexcept {
  return state == AttemptState::verified || state == AttemptState::failed ||
         state == AttemptState::aborted || state == AttemptState::rolled_back ||
         state == AttemptState::abandoned;
}

bool attempt_state_is_in_flight(AttemptState state) noexcept {
  // "In flight" means an operation is executing right now. Resting states such
  // as prepared/activated are not in flight: they are waiting for the next
  // stage of the lifecycle to be admitted.
  return state == AttemptState::preparing || state == AttemptState::activating ||
         state == AttemptState::verifying || state == AttemptState::rolling_back;
}

bool attempt_state_is_recoverable(AttemptState state) noexcept {
  return state == AttemptState::prepared || state == AttemptState::activated ||
         state == AttemptState::verified;
}

const char* stage_state_name(StageState state) noexcept {
  switch (state) {
    case StageState::pending: return "pending";
    case StageState::preparing: return "preparing";
    case StageState::prepared: return "prepared";
    case StageState::activating: return "activating";
    case StageState::activating_partial: return "activating-partial";
    case StageState::verifying: return "verifying";
    case StageState::completed: return "completed";
    case StageState::failed: return "failed";
    case StageState::blocked: return "blocked";
    case StageState::skipped: return "skipped";
  }
  return "unknown";
}

bool stage_state_is_terminal(StageState state) noexcept {
  return state == StageState::completed || state == StageState::failed ||
         state == StageState::skipped;
}

std::string TransitionVerdict::explain() const {
  if (allowed) {
    return "allowed";
  }
  std::string out = reason;
  if (!unmet.empty()) {
    out.append(" (unmet: ");
    for (std::size_t i = 0; i < unmet.size(); ++i) {
      if (i != 0) {
        out.append(", ");
      }
      out.append(unmet[i]);
    }
    out.append(")");
  }
  return out;
}

TransitionVerdict evaluate_transition(UpgradeState from, UpgradeState to,
                                      const TransitionGuards& guards) {
  TransitionVerdict verdict;
  if (from == to) {
    verdict.allowed = true;
    verdict.reason = "no-op transition";
    return verdict;
  }
  if (upgrade_state_is_terminal(from)) {
    verdict.allowed = false;
    verdict.reason = std::string("state ") + upgrade_state_name(from) + " is terminal";
    verdict.unmet.push_back("terminal-state");
    return verdict;
  }
  for (const auto& rule : kRules) {
    if (rule.from != from || rule.to != to) {
      continue;
    }
    for (std::size_t i = 0; i < rule.requirement_count; ++i) {
      auto member = rule.requirements[i];
      if (member == nullptr) {
        continue;
      }
      if (!guard_value(guards, member)) {
        verdict.unmet.push_back(describe_guard(member));
      }
    }
    // Operator transitions additionally accept an explicit manual release.
    if (verdict.unmet.empty()) {
      verdict.allowed = true;
      verdict.reason = rule.description;
      return verdict;
    }
    verdict.allowed = false;
    verdict.reason = rule.description;
    return verdict;
  }
  verdict.allowed = false;
  verdict.reason = std::string("transition ") + upgrade_state_name(from) + " -> " +
                   upgrade_state_name(to) + " is not part of the lifecycle";
  verdict.unmet.push_back("undefined-transition");
  return verdict;
}

std::vector<UpgradeState> allowed_transitions(UpgradeState from) {
  std::vector<UpgradeState> out;
  for (std::size_t i = 0; i < kUpgradeStateCount; ++i) {
    const auto to = static_cast<UpgradeState>(i);
    if (to == from) {
      continue;
    }
    for (const auto& rule : kRules) {
      if (rule.from == from && rule.to == to) {
        out.push_back(to);
        break;
      }
    }
  }
  return out;
}

}  // namespace fum
