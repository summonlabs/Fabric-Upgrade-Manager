// Deterministic plan construction. Pure: no adapter or port calls happen here.
#pragma once

#include <string>
#include <vector>

#include "fum/model/campaign.hpp"
#include "fum/model/policy.hpp"
#include "fum/model/target.hpp"

namespace fum {

struct [[nodiscard]] PlannerInput {
  UpgradePlanRequest request;
  InventorySnapshot inventory;
  const Policy* policy = nullptr;
  Timestamp now;
};

// Builds the plan the runtime will execute. Rejections are collected, never
// thrown: an executable plan is one with an empty rejection list.
[[nodiscard]] Result<UpgradePlan> build_plan(const PlannerInput& input);

}  // namespace fum
