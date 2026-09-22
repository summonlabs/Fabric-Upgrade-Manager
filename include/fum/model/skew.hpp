// Version skew budgets for staged upgrades.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fum/core/json.hpp"
#include "fum/model/version.hpp"

namespace fum {

// A live target may trail the newest version in flight by at most this many
// major, minor and patch releases. Zero major skew means no cross-major mix is
// permitted at all.
struct [[nodiscard]] SkewBudget {
  std::uint32_t max_major_skew = 0;
  std::uint32_t max_minor_skew = 1;
  std::uint32_t max_patch_skew = 8;
  // Explicit waivers for pairs the operator has accepted, regardless of limits.
  std::vector<std::pair<Version, Version>> allowed_pairs;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] bool pair_allowed(const Version& a, const Version& b) const;
  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<SkewBudget> from_json(const json::Value& value);
};

struct [[nodiscard]] SkewViolation {
  std::string subject;
  std::string detail;
  std::int64_t distance = 0;
};

struct [[nodiscard]] SkewAssessment {
  bool within_budget = true;
  // Normalised distance score: major * 1000000 + minor * 1000 + patch.
  std::uint32_t worst_distance = 0;
  std::vector<SkewViolation> violations;
  std::string summary;

  [[nodiscard]] json::Value to_json() const;
};

// Assesses the live mix against the newest version in flight. Deterministic:
// violations are emitted in subject order.
[[nodiscard]] SkewAssessment assess_skew(const SkewBudget& budget,
                                         const std::vector<std::pair<std::string, Version>>& live,
                                         const Version& newest);

}  // namespace fum
