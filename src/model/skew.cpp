#include "fum/model/skew.hpp"

#include <algorithm>

namespace fum {
namespace {

std::uint32_t normalized_distance(const Version& observed, const Version& newest) {
  const std::uint32_t major =
      newest.major() > observed.major() ? newest.major() - observed.major() : 0;
  const std::uint32_t minor =
      newest.minor() > observed.minor() ? newest.minor() - observed.minor() : 0;
  const std::uint32_t patch =
      newest.patch() > observed.patch() ? newest.patch() - observed.patch() : 0;
  return major * 1000000u + minor * 1000u + patch;
}

}  // namespace

Status SkewBudget::validate() const {
  if (max_major_skew > 64 || max_minor_skew > 4096 || max_patch_skew > 100000) {
    return make_error(ErrorCode::invalid_argument, "skew limits exceed the permitted bounds");
  }
  if (allowed_pairs.size() > 64) {
    return make_error(ErrorCode::invalid_argument, "too many skew waivers declared");
  }
  for (const auto& pair : allowed_pairs) {
    if (pair.first.empty() || pair.second.empty()) {
      return make_error(ErrorCode::invalid_argument, "a skew waiver names an empty version");
    }
  }
  return ok_status();
}

bool SkewBudget::pair_allowed(const Version& a, const Version& b) const {
  for (const auto& pair : allowed_pairs) {
    if ((pair.first == a && pair.second == b) || (pair.first == b && pair.second == a)) {
      return true;
    }
  }
  return false;
}

json::Value SkewBudget::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("max_major_skew", json::Value::make_uint(max_major_skew));
  value.set("max_minor_skew", json::Value::make_uint(max_minor_skew));
  value.set("max_patch_skew", json::Value::make_uint(max_patch_skew));
  json::Value pairs = json::Value::make_array();
  for (const auto& pair : allowed_pairs) {
    json::Value entry = json::Value::make_object();
    entry.set("from", json::Value::make_string(pair.first.text()));
    entry.set("to", json::Value::make_string(pair.second.text()));
    pairs.push(std::move(entry));
  }
  value.set("allowed_pairs", std::move(pairs));
  return value;
}

Result<SkewBudget> SkewBudget::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "skew budget must be a json object");
  }
  SkewBudget out;
  struct Field {
    const char* key;
    std::uint32_t* target;
  };
  const Field fields[] = {{"max_major_skew", &out.max_major_skew},
                          {"max_minor_skew", &out.max_minor_skew},
                          {"max_patch_skew", &out.max_patch_skew}};
  for (const auto& entry : fields) {
    const json::Value* item = value.find(entry.key);
    if (item == nullptr) {
      continue;
    }
    std::uint64_t parsed = 0;
    FUM_TRY(parsed, item->as_uint());
    if (parsed > 0xFFFFFFFFull) {
      return make_error(ErrorCode::invalid_argument, "skew limit is out of range", entry.key);
    }
    *entry.target = static_cast<std::uint32_t>(parsed);
  }
  const json::Value* pairs = value.find("allowed_pairs");
  if (pairs != nullptr) {
    if (!pairs->is_array()) {
      return make_error(ErrorCode::invalid_argument, "allowed_pairs must be an array");
    }
    for (const auto& item : pairs->items()) {
      std::string_view from_text;
      std::string_view to_text;
      FUM_TRY(from_text, item.require_string("from"));
      FUM_TRY(to_text, item.require_string("to"));
      Version from;
      Version to;
      FUM_TRY(from, Version::parse(from_text));
      FUM_TRY(to, Version::parse(to_text));
      out.allowed_pairs.emplace_back(from, to);
    }
  }
  FUM_TRYV(out.validate());
  return out;
}

json::Value SkewAssessment::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("within_budget", json::Value::make_bool(within_budget));
  value.set("worst_distance", json::Value::make_uint(worst_distance));
  value.set("summary", json::Value::make_string(summary));
  json::Value list = json::Value::make_array();
  for (const auto& violation : violations) {
    json::Value entry = json::Value::make_object();
    entry.set("subject", json::Value::make_string(violation.subject));
    entry.set("detail", json::Value::make_string(violation.detail));
    entry.set("distance", json::Value::make_int(violation.distance));
    list.push(std::move(entry));
  }
  value.set("violations", std::move(list));
  return value;
}

SkewAssessment assess_skew(const SkewBudget& budget,
                           const std::vector<std::pair<std::string, Version>>& live,
                           const Version& newest) {
  SkewAssessment assessment;
  std::vector<std::pair<std::string, Version>> ordered = live;
  std::sort(ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::size_t mixed = 0;
  for (const auto& entry : ordered) {
    const Version& observed = entry.second;
    if (observed.empty()) {
      assessment.within_budget = false;
      SkewViolation violation;
      violation.subject = entry.first;
      violation.detail = "the target reports no version while a campaign is in flight";
      assessment.violations.push_back(std::move(violation));
      continue;
    }
    if (observed == newest) {
      continue;
    }
    ++mixed;
    if (budget.pair_allowed(observed, newest)) {
      assessment.worst_distance =
          std::max(assessment.worst_distance, normalized_distance(observed, newest));
      continue;
    }
    const std::uint32_t major_delta =
        newest.major() > observed.major() ? newest.major() - observed.major() : 0;
    const std::uint32_t minor_delta =
        newest.minor() > observed.minor() ? newest.minor() - observed.minor() : 0;
    const std::uint32_t patch_delta =
        newest.patch() > observed.patch() ? newest.patch() - observed.patch() : 0;
    const bool ahead_of_campaign = observed > newest;
    const std::uint32_t distance = normalized_distance(observed, newest);
    assessment.worst_distance = std::max(assessment.worst_distance, distance);

    if (ahead_of_campaign) {
      assessment.within_budget = false;
      SkewViolation violation;
      violation.subject = entry.first;
      violation.detail = "the live target runs " + observed.text() +
                         ", which is newer than the campaign artifact " + newest.text();
      violation.distance = static_cast<std::int64_t>(distance);
      assessment.violations.push_back(std::move(violation));
      continue;
    }
    if (major_delta > budget.max_major_skew) {
      assessment.within_budget = false;
      SkewViolation violation;
      violation.subject = entry.first;
      violation.detail = "major skew " + std::to_string(major_delta) + " exceeds the budget of " +
                         std::to_string(budget.max_major_skew) + " (" + observed.text() + " vs " +
                         newest.text() + ")";
      violation.distance = static_cast<std::int64_t>(distance);
      assessment.violations.push_back(std::move(violation));
      continue;
    }
    if (major_delta == 0 && minor_delta > budget.max_minor_skew) {
      assessment.within_budget = false;
      SkewViolation violation;
      violation.subject = entry.first;
      violation.detail = "minor skew " + std::to_string(minor_delta) + " exceeds the budget of " +
                         std::to_string(budget.max_minor_skew) + " (" + observed.text() + " vs " +
                         newest.text() + ")";
      violation.distance = static_cast<std::int64_t>(distance);
      assessment.violations.push_back(std::move(violation));
      continue;
    }
    if (major_delta == 0 && minor_delta == 0 && patch_delta > budget.max_patch_skew) {
      assessment.within_budget = false;
      SkewViolation violation;
      violation.subject = entry.first;
      violation.detail = "patch skew " + std::to_string(patch_delta) + " exceeds the budget of " +
                         std::to_string(budget.max_patch_skew) + " (" + observed.text() + " vs " +
                         newest.text() + ")";
      violation.distance = static_cast<std::int64_t>(distance);
      assessment.violations.push_back(std::move(violation));
      continue;
    }
  }
  if (assessment.within_budget) {
    assessment.summary = "skew within budget: " + std::to_string(mixed) +
                         " target(s) on a different version, worst normalized distance " +
                         std::to_string(assessment.worst_distance) + " (limits " +
                         std::to_string(budget.max_major_skew) + " major / " +
                         std::to_string(budget.max_minor_skew) + " minor / " +
                         std::to_string(budget.max_patch_skew) + " patch)";
  } else {
    assessment.summary = "skew budget exceeded with " +
                         std::to_string(assessment.violations.size()) + " violation(s)";
  }
  return assessment;
}

}  // namespace fum
