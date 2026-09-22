#include "fum/engine/preflight.hpp"

#include <algorithm>

#include "fum/core/hash.hpp"

namespace fum {

PreflightTicket PreflightReport::ticket() const {
  PreflightTicket out;
  out.campaign = campaign;
  out.generation = generation;
  out.incarnation = incarnation;
  out.epoch = epoch;
  out.issued_at = issued_at;
  out.expires_at = expires_at;
  out.fingerprint = fingerprint;
  out.passed = passed;
  out.authority = authority;   // the authority that issued the ticket
  out.policy = make_PolicyId(policy);
  out.policy_revision = policy_revision;
  return out;
}

json::Value PreflightReport::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("campaign_id", json::Value::make_string(campaign.str()));
  value.set("generation", json::Value::make_uint(generation.value()));
  value.set("incarnation", json::Value::make_uint(incarnation.value()));
  value.set("epoch", json::Value::make_uint(epoch.value()));
  value.set("authority", json::Value::make_string(authority.str()));
  value.set("passed", json::Value::make_bool(passed));
  value.set("issued_at", json::Value::make_string(issued_at.to_iso8601()));
  value.set("expires_at", json::Value::make_string(expires_at.to_iso8601()));
  value.set("integrity", integrity.to_json());
  json::Value targets_json = json::Value::make_array();
  for (const auto& target : targets) {
    targets_json.push(target.to_json());
  }
  value.set("targets", std::move(targets_json));
  json::Value blockers_json = json::Value::make_array();
  for (const auto& blocker : blockers) {
    blockers_json.push(json::Value::make_string(blocker));
  }
  value.set("blockers", std::move(blockers_json));
  value.set("skew", skew.to_json());
  value.set("fingerprint", json::Value::make_string(fingerprint));
  value.set("policy", json::Value::make_string(policy));
  value.set("policy_revision", json::Value::make_uint(policy_revision.value()));
  return value;
}

PreflightReport run_preflight(const PreflightInput& input) {
  PreflightReport report;
  if (input.campaign == nullptr || input.plan == nullptr || input.policy == nullptr) {
    report.blockers.push_back("preflight requires a campaign, a plan and a policy");
    return report;
  }
  const CampaignRecord& campaign = *input.campaign;
  const UpgradePlan& plan = *input.plan;
  const Policy& policy = *input.policy;

  report.campaign = campaign.id;
  report.generation = campaign.generation;
  report.incarnation = input.incarnation;
  report.epoch = input.epoch;
  report.authority = campaign.authority;
  report.issued_at = input.now;
  report.expires_at = input.now.plus(policy.preflight_validity);
  report.integrity = input.integrity;
  report.policy = policy.id.str();
  report.policy_revision = policy.revision;

  if (!plan.executable()) {
    for (const auto& rejection : plan.rejections) {
      report.blockers.push_back(std::string("plan rejected: ") + plan_rejection_name(rejection.code) +
                                " for " + rejection.subject + ": " + rejection.detail);
    }
  }
  if (!input.artifact_source_integrated) {
    report.blockers.push_back(
        "artifact source is not integrated: artifact bytes cannot be resolved or verified");
  } else if (!input.payload.present) {
    report.blockers.push_back("artifact payload is unavailable: " + input.payload.detail);
  }
  if (!input.integrity.verified) {
    report.blockers.push_back("artifact integrity is not verified: " + input.integrity.detail);
  }
  if (policy.require_signature_declaration && !plan.artifact.provenance().declared()) {
    report.blockers.push_back(
        "the artifact declares no signer provenance and the policy requires it");
  }
  if (!input.registry_integrated) {
    report.blockers.push_back(
        "the Fabric Compatibility Registry is not integrated: compatibility cannot be asserted");
  }
  if (!input.compatibility_evidence_fresh) {
    report.blockers.push_back("compatibility evidence is not fresh");
  }

  // Planned skew: the mix that would exist if nothing had been upgraded yet.
  std::vector<std::pair<std::string, Version>> live;
  for (const auto& target : input.inventory.targets) {
    if (target.component == plan.component) {
      live.emplace_back(target.id.str(), target.installed_version);
    }
  }
  report.skew = assess_skew(plan.skew_budget, live, plan.artifact.version());

  std::vector<const TargetDescriptor*> planned;
  for (const auto& stage : plan.stages) {
    for (const auto& target : stage.targets) {
      const TargetDescriptor* descriptor = input.inventory.find(target);
      if (descriptor != nullptr) {
        planned.push_back(descriptor);
      }
    }
  }
  std::sort(planned.begin(), planned.end(),
            [](const TargetDescriptor* a, const TargetDescriptor* b) { return a->id < b->id; });

  bool requires_drain = false;
  for (const auto& stage : plan.stages) {
    if (stage.requires_service_removal) {
      requires_drain = true;
    }
  }
  if (requires_drain && !input.drain_integrated) {
    report.blockers.push_back(
        "the plan requires service removal but the Drain/Maintenance Fabric is not integrated");
  }

  for (const auto* target : planned) {
    PreflightTargetVerdict verdict;
    verdict.target = target->id;
    verdict.integrity_verified = input.integrity.verified;
    verdict.platform_supported = plan.artifact.supported_on(target->platform);
    verdict.strategy_supported = target->claims.supports(plan.strategy);
    verdict.capabilities_satisfied = true;
    for (const auto& requirement : plan.artifact.capabilities()) {
      if (std::find(target->capabilities.begin(), target->capabilities.end(), requirement.name) ==
          target->capabilities.end()) {
        verdict.capabilities_satisfied = false;
        verdict.reasons.push_back("missing capability " + requirement.name);
      }
    }
    const auto found = input.verdicts.find(target->id.str());
    if (!input.registry_integrated) {
      verdict.reasons.push_back("compatibility registry is not integrated");
    } else if (found == input.verdicts.end() || !found->second.answered) {
      verdict.reasons.push_back("the compatibility registry did not answer for this target");
    } else {
      const CompatibilityVerdict& registry = found->second;
      verdict.compatible = registry.compatible;
      verdict.rollback_available = registry.rollback_supported ||
                                   (!plan.artifact.rollback().rollback_artifact.empty()) ||
                                   plan.artifact.rollback().reversible;
      for (const auto& blocker : registry.blockers) {
        verdict.reasons.push_back("registry: " + blocker);
      }
      if (!registry.compatible) {
        verdict.reasons.push_back("registry revision " + registry.registry_revision +
                                  " declares this upgrade incompatible");
      }
    }
    if (!verdict.platform_supported) {
      verdict.reasons.push_back("artifact does not declare platform " + target->platform);
    }
    if (!verdict.strategy_supported) {
      verdict.reasons.push_back(std::string("adapter does not claim strategy ") +
                                strategy_kind_name(plan.strategy));
    }
    if (!verdict.rollback_available) {
      verdict.reasons.push_back(
          "no rollback path is declared for this target; the campaign may still proceed but "
          "rollback will be refused");
    }
    report.targets.push_back(std::move(verdict));
  }
  if (report.targets.empty()) {
    report.blockers.push_back("no target could be resolved from the current inventory");
  }
  for (const auto& verdict : report.targets) {
    if (!verdict.admissible()) {
      report.blockers.push_back("target " + verdict.target.str() + " is not admissible: " +
                                (verdict.reasons.empty() ? std::string("unspecified")
                                                         : verdict.reasons.front()));
    }
  }
  if (!report.skew.within_budget) {
    report.blockers.push_back("the planned version mix exceeds the skew budget: " +
                              report.skew.summary);
  }

  report.passed = report.blockers.empty();

  json::Value material = json::Value::make_object();
  material.set("campaign", json::Value::make_string(report.campaign.str()));
  material.set("generation", json::Value::make_uint(report.generation.value()));
  material.set("epoch", json::Value::make_uint(report.epoch.value()));
  material.set("artifact", json::Value::make_string(plan.artifact.digest().hex()));
  material.set("inventory_revision", json::Value::make_uint(input.inventory.revision.value()));
  material.set("passed", json::Value::make_bool(report.passed));
  json::Value verdicts_json = json::Value::make_array();
  for (const auto& verdict : report.targets) {
    verdicts_json.push(verdict.to_json());
  }
  material.set("targets", std::move(verdicts_json));
  json::Value blockers_json = json::Value::make_array();
  for (const auto& blocker : report.blockers) {
    blockers_json.push(json::Value::make_string(blocker));
  }
  material.set("blockers", std::move(blockers_json));
  report.fingerprint = hash::sha256_hex(material.dump());
  return report;
}

}  // namespace fum
