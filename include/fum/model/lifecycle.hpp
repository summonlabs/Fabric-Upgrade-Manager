// Upgrade lifecycle states, guard conditions and deterministic transitions.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fum/core/result.hpp"

namespace fum {

// Campaign-level lifecycle. Every state is reachable only through
// evaluate_transition with its guards satisfied.
enum class UpgradeState : std::uint8_t {
  proposed = 0,
  validated,
  prepared,
  staged,
  activating,
  verifying,
  completed,
  blocked,
  paused,
  failed,
  rollback_planned,
  rolling_back,
  rolled_back,
};

inline constexpr std::size_t kUpgradeStateCount = 13;

[[nodiscard]] const char* upgrade_state_name(UpgradeState state) noexcept;
[[nodiscard]] Result<UpgradeState> parse_upgrade_state(std::string_view text);
[[nodiscard]] bool upgrade_state_is_terminal(UpgradeState state) noexcept;
[[nodiscard]] bool upgrade_state_is_active(UpgradeState state) noexcept;
[[nodiscard]] bool upgrade_state_permits_pause(UpgradeState state) noexcept;
[[nodiscard]] bool upgrade_state_is_pre_activation(UpgradeState state) noexcept;

enum class AttemptState : std::uint8_t {
  created = 0,
  preparing,
  prepared,
  activating,
  activated,
  verifying,
  verified,
  failed,
  aborted,
  rolled_back,
  abandoned,
  rolling_back,
};

[[nodiscard]] const char* attempt_state_name(AttemptState state) noexcept;
[[nodiscard]] bool attempt_state_is_terminal(AttemptState state) noexcept;
[[nodiscard]] bool attempt_state_is_in_flight(AttemptState state) noexcept;
[[nodiscard]] bool attempt_state_is_recoverable(AttemptState state) noexcept;

enum class StageState : std::uint8_t {
  pending = 0,
  preparing,
  prepared,
  activating,
  activating_partial,
  verifying,
  completed,
  failed,
  blocked,
  skipped,
};

[[nodiscard]] const char* stage_state_name(StageState state) noexcept;
[[nodiscard]] bool stage_state_is_terminal(StageState state) noexcept;

// Everything a transition depends on. The engine computes these from evidence
// and policy; the lifecycle layer only decides whether the move is legal.
// Every guard defaults to "not proven": a transition is refused unless the
// caller has established the precondition. Nothing is permissive by accident.
struct [[nodiscard]] TransitionGuards {
  bool preflight_current = false;
  bool artifact_integrity_verified = false;
  bool rollback_plan_recorded = false;
  bool irreversible_acknowledged = false;
  bool adapter_supports_strategy = false;
  bool staging_complete = false;
  bool activation_complete = false;
  bool verification_fresh_passed = false;
  bool skew_within_budget = false;
  bool drain_satisfied = false;
  bool all_stages_complete = false;
  bool failure_recorded = false;
  bool rollback_eligible = false;
  bool rollback_complete = false;
  bool manual_release_required = false;   // operator cleared a blocked campaign
};

struct [[nodiscard]] TransitionVerdict {
  bool allowed = false;
  std::vector<std::string> unmet;
  std::string reason;

  [[nodiscard]] std::string explain() const;
};

[[nodiscard]] TransitionVerdict evaluate_transition(UpgradeState from, UpgradeState to,
                                                    const TransitionGuards& guards);
[[nodiscard]] std::vector<UpgradeState> allowed_transitions(UpgradeState from);

}  // namespace fum
