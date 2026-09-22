#include "fum/engine/planner.hpp"

#include <algorithm>
#include <map>

namespace fum {
namespace {

void reject(UpgradePlan& plan, PlanRejectionCode code, std::string subject, std::string detail) {
  PlanRejection rejection;
  rejection.code = code;
  rejection.subject = std::move(subject);
  rejection.detail = std::move(detail);
  plan.rejections.push_back(std::move(rejection));
}

bool has_capability(const TargetDescriptor& target, const std::string& name) {
  return std::find(target.capabilities.begin(), target.capabilities.end(), name) !=
         target.capabilities.end();
}

std::vector<std::string> missing_capabilities(const TargetDescriptor& target,
                                              const ArtifactDescriptor& artifact) {
  std::vector<std::string> missing;
  for (const auto& requirement : artifact.capabilities()) {
    if (!has_capability(target, requirement.name)) {
      missing.push_back(requirement.name);
    }
  }
  return missing;
}

StagePlan make_stage(const std::string& campaign_id, std::size_t index,
                     const std::vector<const TargetDescriptor*>& targets,
                     const ArtifactDescriptor& artifact, StrategyKind strategy,
                     std::uint32_t max_parallel) {
  StagePlan stage;
  stage.id = make_StageId(campaign_id + ":stage-" + std::to_string(index + 1));
  stage.name = "stage-" + std::to_string(index + 1);
  stage.max_parallel = max_parallel == 0 ? 1 : max_parallel;
  for (const auto* target : targets) {
    stage.targets.push_back(target->id);
    if (stage.redundancy_group.empty() && !target->redundancy_group.empty()) {
      stage.redundancy_group = target->redundancy_group;
    }
    if (target->claims.requires_service_removal) {
      stage.requires_service_removal = true;
    }
  }

  PlanStep prepare;
  prepare.id = make_StepId(stage.id.str() + "/prepare");
  prepare.action = "prepare";
  prepare.description = "deliver and verify the artifact on the target";
  stage.steps.push_back(std::move(prepare));

  PlanStep activate;
  activate.id = make_StepId(stage.id.str() + "/activate");
  activate.action = "activate";
  activate.description = std::string("activate the artifact with strategy ") +
                         strategy_kind_name(strategy);
  const auto& rollback = artifact.rollback();
  if (!rollback.reversible) {
    activate.irreversible = true;
    activate.irreversible_reason = rollback.boundary.empty()
                                       ? "the artifact declares the upgrade irreversible"
                                       : rollback.boundary;
  }
  for (const auto& step : rollback.irreversible_steps) {
    if (step.str() == activate.id.str() || step.str() == "activate") {
      activate.irreversible = true;
      activate.irreversible_reason =
          rollback.boundary.empty() ? "declared irreversible step" : rollback.boundary;
    }
  }
  stage.steps.push_back(std::move(activate));

  if (strategy == StrategyKind::control_plane_generation_handoff) {
    PlanStep handoff;
    handoff.id = make_StepId(stage.id.str() + "/generation-handoff");
    handoff.action = "generation-handoff";
    handoff.description = "hand control-plane authority to the new generation";
    handoff.irreversible = true;
    handoff.irreversible_reason =
        "a completed generation handoff cannot be reverted in place; the previous generation is "
        "retired";
    stage.steps.push_back(std::move(handoff));
  }

  PlanStep verify;
  verify.id = make_StepId(stage.id.str() + "/verify");
  verify.action = "verify";
  verify.description = "post-activation verification with fresh health and version evidence";
  stage.steps.push_back(std::move(verify));

  PlanStep rollback_step;
  rollback_step.id = make_StepId(stage.id.str() + "/rollback");
  rollback_step.action = "rollback";
  rollback_step.description = "return the target to its previous version";
  stage.steps.push_back(std::move(rollback_step));
  return stage;
}

}  // namespace

Result<UpgradePlan> build_plan(const PlannerInput& input) {
  if (input.policy == nullptr) {
    return make_error(ErrorCode::invalid_argument, "planner requires a policy");
  }
  const Policy& policy = *input.policy;
  const UpgradePlanRequest& request = input.request;

  UpgradePlan plan;
  plan.campaign = request.campaign;
  plan.generation = Generation(1);
  plan.authority = request.authority;
  plan.component = request.component;
  plan.artifact = request.artifact;
  plan.strategy = request.strategy;
  plan.skew_budget = request.skew_budget;
  plan.policy = policy.id;
  plan.policy_revision = policy.revision;
  plan.created_at = input.now;
  plan.description = request.description;

  if (plan.campaign.empty()) {
    return make_error(ErrorCode::invalid_argument, "plan request must carry a campaign id");
  }
  if (plan.authority.empty()) {
    return make_error(ErrorCode::invalid_argument, "plan request must carry an authority");
  }
  FUM_TRYV(plan.skew_budget.validate());
  FUM_TRYV(request.artifact.validate());
  if (request.artifact.component() != request.component) {
    reject(plan, PlanRejectionCode::artifact_component_mismatch, request.component.str(),
           "the artifact belongs to component " + request.artifact.component().str());
  }

  // Resolve the target set deterministically.
  std::vector<const TargetDescriptor*> selected;
  if (request.targets.empty()) {
    for (const auto& target : input.inventory.targets) {
      if (target.component == request.component) {
        selected.push_back(&target);
      }
    }
  } else {
    for (const auto& requested : request.targets) {
      const TargetDescriptor* target = input.inventory.find(requested);
      if (target == nullptr) {
        reject(plan, PlanRejectionCode::unknown_target, requested.str(),
               "the target is not present in the resolved inventory");
        continue;
      }
      selected.push_back(target);
    }
  }
  std::sort(selected.begin(), selected.end(),
            [](const TargetDescriptor* a, const TargetDescriptor* b) { return a->id < b->id; });
  for (std::size_t i = 1; i < selected.size(); ++i) {
    if (selected[i]->id == selected[i - 1]->id) {
      reject(plan, PlanRejectionCode::duplicate_target, selected[i]->id.str(),
             "the same target was requested more than once");
    }
  }
  if (selected.empty()) {
    reject(plan, PlanRejectionCode::no_targets, request.component.str(),
           "no target matches the requested component");
  }
  if (selected.size() > policy.max_targets) {
    reject(plan, PlanRejectionCode::target_limit_exceeded, request.component.str(),
           "the plan covers " + std::to_string(selected.size()) + " targets but the policy permits " +
               std::to_string(policy.max_targets));
  }

  const Version& to_version = request.artifact.version();
  for (const auto* target : selected) {
    if (target->component != request.component) {
      reject(plan, PlanRejectionCode::mixed_component, target->id.str(),
             "target belongs to component " + target->component.str());
    }
    if (!request.artifact.supported_on(target->platform)) {
      reject(plan, PlanRejectionCode::platform_unsupported, target->id.str(),
             "the artifact does not declare platform " + target->platform);
    }
    const auto missing = missing_capabilities(*target, request.artifact);
    if (!missing.empty()) {
      std::string detail = "missing capabilities:";
      for (const auto& name : missing) {
        detail.append(" ");
        detail.append(name);
      }
      reject(plan, PlanRejectionCode::capability_missing, target->id.str(), std::move(detail));
    }
    if (!target->claims.supports(request.strategy)) {
      reject(plan, PlanRejectionCode::strategy_unsupported, target->id.str(),
             std::string("the adapter does not claim strategy ") +
                 strategy_kind_name(request.strategy));
    }
    if (!target->installed_version.empty() && target->installed_version > to_version) {
      reject(plan, PlanRejectionCode::version_regression, target->id.str(),
             "installed version " + target->installed_version.text() +
                 " is newer than the requested " + to_version.text());
    }
    if (target->installed_version == to_version && !to_version.empty()) {
      reject(plan, PlanRejectionCode::already_at_version, target->id.str(),
             "the target already runs " + to_version.text());
    }
  }

  // Stage construction.
  std::vector<StagePlan> stages;
  switch (request.strategy) {
    case StrategyKind::in_place:
    case StrategyKind::control_plane_generation_handoff: {
      stages.push_back(make_stage(plan.campaign.str(), 0, selected, request.artifact,
                                  request.strategy, request.max_parallel_activations));
      break;
    }
    case StrategyKind::restart_based: {
      for (std::size_t i = 0; i < selected.size(); ++i) {
        stages.push_back(make_stage(plan.campaign.str(), i, {selected[i]}, request.artifact,
                                    request.strategy, 1));
      }
      break;
    }
    case StrategyKind::redundant_pair_rolling: {
      // Group by redundancy group, then interleave so no group is upgraded
      // twice in a row: stage k takes the k-th member of every group.
      std::map<std::string, std::vector<const TargetDescriptor*>> groups;
      for (const auto* target : selected) {
        if (target->redundancy_group.empty()) {
          reject(plan, PlanRejectionCode::strategy_unsupported, target->id.str(),
                 "rolling replacement requires the target to belong to a redundancy group");
          continue;
        }
        groups[target->redundancy_group].push_back(target);
      }
      std::size_t index = 0;
      bool progressing = true;
      while (progressing) {
        progressing = false;
        std::vector<const TargetDescriptor*> wave;
        for (auto& entry : groups) {
          if (index < entry.second.size()) {
            wave.push_back(entry.second[index]);
            progressing = true;
          }
        }
        if (!wave.empty()) {
          stages.push_back(make_stage(plan.campaign.str(), index, wave, request.artifact,
                                      request.strategy, request.max_parallel_activations));
        }
        ++index;
      }
      for (const auto& entry : groups) {
        if (entry.second.size() < 2) {
          reject(plan, PlanRejectionCode::strategy_unsupported, entry.first,
                 "redundant-pair rolling requires at least two members in the group");
        }
      }
      break;
    }
  }

  if (stages.size() > policy.max_stages) {
    reject(plan, PlanRejectionCode::stage_limit_exceeded, request.component.str(),
           "the plan needs " + std::to_string(stages.size()) + " stages but the policy permits " +
               std::to_string(policy.max_stages));
  }
  for (const auto& stage : stages) {
    for (const auto& step : stage.steps) {
      if (step.irreversible) {
        plan.irreversible_steps.push_back(step.id);
      }
    }
  }
  plan.stages = std::move(stages);

  if (!plan.irreversible_steps.empty() && !request.acknowledge_irreversible_steps) {
    reject(plan, PlanRejectionCode::irreversible_unacknowledged, plan.campaign.str(),
           "the plan contains " + std::to_string(plan.irreversible_steps.size()) +
               " irreversible step(s) that were not acknowledged by the operator");
  }
  return plan;
}

}  // namespace fum
