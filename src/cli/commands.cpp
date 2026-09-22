#include "commands.hpp"

#include <algorithm>

#include "fum/engine/engine.hpp"

namespace fum::cli {
namespace {

int exit_code_for(const Error& error) {
  switch (error.code()) {
    case ErrorCode::not_found:
      return 4;
    case ErrorCode::incompatible:
    case ErrorCode::irreversible:
    case ErrorCode::policy_denied:
    case ErrorCode::precondition_failed:
    case ErrorCode::stale:
    case ErrorCode::fenced:
      return 3;
    case ErrorCode::not_integrated:
    case ErrorCode::unsupported:
      return 5;
    default:
      return 1;
  }
}

int report_error(const CommandContext& context, const Error& error) {
  if (context.json) {
    json::Value value = json::Value::make_object();
    value.set("ok", json::Value::make_bool(false));
    value.set("error_code", json::Value::make_string(error_code_name(error.code())));
    value.set("message", json::Value::make_string(error.message()));
    value.set("detail", json::Value::make_string(error.detail()));
    *context.out << value.dump() << "\n";
  } else {
    *context.err << "error: " << error.to_string() << "\n";
  }
  return exit_code_for(error);
}

Result<UpgradePlanRequest> load_request(const std::string& path) {
  if (path.empty()) {
    return make_error(ErrorCode::invalid_argument, "a plan request file is required (--request)");
  }
  auto text = fs::read_file(path, 4ull * 1024 * 1024);
  if (!text.has_value()) {
    return text.error();
  }
  auto document = json::parse_object(text.value());
  if (!document.has_value()) {
    return document.error();
  }
  return UpgradePlanRequest::from_json(document.value());
}

void print_plan(const CommandContext& context, const UpgradePlan& plan) {
  if (context.json) {
    *context.out << plan.to_json().dump_pretty() << "\n";
    return;
  }
  *context.out << "plan for campaign " << plan.campaign.str() << "\n";
  *context.out << "  component:   " << plan.component.str() << "\n";
  *context.out << "  artifact:    " << plan.artifact.id().str() << " "
               << plan.artifact.version().text() << " build " << plan.artifact.build().str()
               << "\n";
  *context.out << "  digest:      " << plan.artifact.digest().hex() << "\n";
  *context.out << "  strategy:    " << strategy_kind_name(plan.strategy) << "\n";
  *context.out << "  authority:   " << plan.authority.str() << "\n";
  *context.out << "  policy:      " << plan.policy.str() << "@" << plan.policy_revision.value()
               << "\n";
  *context.out << "  fingerprint: " << plan.fingerprint() << "\n";
  *context.out << "  stages:      " << plan.stages.size() << "\n";
  for (std::size_t i = 0; i < plan.stages.size(); ++i) {
    const auto& stage = plan.stages[i];
    *context.out << "    " << (i + 1) << ". " << stage.name << " targets=" << stage.targets.size()
                 << " max_parallel=" << stage.max_parallel;
    if (stage.requires_service_removal) {
      *context.out << " requires-service-removal";
    }
    *context.out << "\n";
    for (const auto& target : stage.targets) {
      *context.out << "        - " << target.str() << "\n";
    }
  }
  if (!plan.irreversible_steps.empty()) {
    *context.out << "  irreversible steps:\n";
    for (const auto& step : plan.irreversible_steps) {
      *context.out << "    - " << step.str() << "\n";
    }
  }
  if (!plan.executable()) {
    *context.out << "  rejections:\n";
    for (const auto& rejection : plan.rejections) {
      *context.out << "    - " << plan_rejection_name(rejection.code) << " [" << rejection.subject
                   << "] " << rejection.detail << "\n";
    }
    *context.out << "result: plan is NOT executable\n";
  } else {
    *context.out << "result: plan is executable\n";
  }
}

void print_preflight(const CommandContext& context, const PreflightReport& report) {
  if (context.json) {
    *context.out << report.to_json().dump_pretty() << "\n";
    return;
  }
  *context.out << "preflight for campaign " << report.campaign.str()
               << " generation " << report.generation.value() << "\n";
  *context.out << "  passed:      " << (report.passed ? "true" : "false") << "\n";
  *context.out << "  integrity:   " << (report.integrity.verified ? "verified" : "not verified")
               << " (" << report.integrity.detail << ")\n";
  *context.out << "  issued:      " << report.issued_at.to_iso8601() << "\n";
  *context.out << "  expires:     " << report.expires_at.to_iso8601() << "\n";
  *context.out << "  fingerprint: " << report.fingerprint << "\n";
  *context.out << "  skew:        " << report.skew.summary << "\n";
  for (const auto& target : report.targets) {
    *context.out << "    target " << target.target.str() << ": "
                 << (target.admissible() ? "admissible" : "not admissible") << "\n";
    for (const auto& reason : target.reasons) {
      *context.out << "        - " << reason << "\n";
    }
  }
  if (!report.blockers.empty()) {
    *context.out << "  blockers:\n";
    for (const auto& blocker : report.blockers) {
      *context.out << "    - " << blocker << "\n";
    }
  }
}

void print_status(const CommandContext& context, const CampaignStatus& status) {
  if (context.json) {
    *context.out << status.to_json().dump_pretty() << "\n";
    return;
  }
  const CampaignRecord& campaign = status.campaign;
  *context.out << campaign.id.str() << ": " << upgrade_state_name(campaign.state)
               << " generation=" << campaign.generation.value()
               << " epoch=" << campaign.epoch.value() << " stage="
               << (campaign.active_stage + 1) << "/" << campaign.stages.size()
               << " in_flight=" << status.in_flight << "\n";
  if (!campaign.block_reason.empty()) {
    *context.out << "  blocked: " << campaign.block_reason << "\n";
  }
  if (!campaign.failure_reason.empty()) {
    *context.out << "  failure: " << campaign.failure_reason << "\n";
  }
  *context.out << "  ticket:  " << status.ticket_detail << "\n";
  *context.out << "  skew:    " << status.skew.summary << "\n";
}

}  // namespace

namespace {

// A preflight ticket is bound to the process incarnation that issued it, so a
// ticket from an earlier process can never be used. The CLI therefore re-runs
// preflight inside the current process when it needs a current ticket.
Status ensure_current_preflight(Engine& engine, const CampaignId& campaign,
                                const CommandContext& context) {
  auto status = engine.status(campaign);
  if (!status.has_value()) {
    return status.error();
  }
  if (status.value().ticket_current) {
    return ok_status();
  }
  auto report = engine.preflight(campaign);
  if (!report.has_value()) {
    return report.error();
  }
  if (!report.value().passed) {
    if (!context.json) {
      *context.err << "preflight refused the campaign:\n";
      for (const auto& blocker : report.value().blockers) {
        *context.err << "  - " << blocker << "\n";
      }
    }
    return make_error(ErrorCode::precondition_failed,
                      "preflight refused the campaign in this process incarnation",
                      report.value().blockers.empty() ? "unspecified"
                                                      : report.value().blockers.front());
  }
  if (!context.json) {
    *context.out << "preflight (re-issued in this process): passed, fingerprint "
                 << report.value().fingerprint.substr(0, 16) << "\n";
  }
  return ok_status();
}

}  // namespace

int run_command(const CommandOptions& options, const CommandContext& context) {
  if (context.engine == nullptr || context.out == nullptr || context.err == nullptr) {
    return 1;
  }
  Engine& engine = *context.engine;

  if (options.command == "plan") {
    auto request = load_request(options.request_path);
    if (!request.has_value()) {
      return report_error(context, request.error());
    }
    auto plan = engine.plan(request.value());
    if (!plan.has_value()) {
      return report_error(context, plan.error());
    }
    print_plan(context, plan.value());
    return plan.value().executable() ? 0 : 3;
  }

  if (options.command == "create") {
    auto request = load_request(options.request_path);
    if (!request.has_value()) {
      return report_error(context, request.error());
    }
    auto plan = engine.create_campaign(request.value());
    if (!plan.has_value()) {
      return report_error(context, plan.error());
    }
    if (context.json) {
      *context.out << plan.value().to_json().dump_pretty() << "\n";
    } else {
      *context.out << "created campaign " << plan.value().campaign.str() << "\n";
    }
    return plan.value().executable() ? 0 : 3;
  }

  if (options.command == "run") {
    auto request = load_request(options.request_path);
    if (!request.has_value()) {
      return report_error(context, request.error());
    }
    auto created = engine.create_campaign(request.value());
    if (!created.has_value()) {
      return report_error(context, created.error());
    }
    auto report = engine.preflight(request.value().campaign);
    if (!report.has_value()) {
      return report_error(context, report.error());
    }
    if (!context.json) {
      print_preflight(context, report.value());
    }
    if (!report.value().passed) {
      return 3;
    }
    StartOptions start_options;
    start_options.allow_irreversible = options.allow_irreversible;
    start_options.max_parallel = options.parallel;
    start_options.reason = options.reason;
    const Status started = engine.start(request.value().campaign, start_options);
    if (!started.has_value()) {
      return report_error(context, started.error());
    }
    const Status ran = engine.run_until_idle();
    if (!ran.has_value()) {
      return report_error(context, ran.error());
    }
    auto status = engine.status(request.value().campaign);
    if (!status.has_value()) {
      return report_error(context, status.error());
    }
    print_status(context, status.value());
    return upgrade_state_is_terminal(status.value().campaign.state) ? 0 : 3;
  }

  if (options.command == "preflight") {
    auto report = engine.preflight(make_CampaignId(options.campaign));
    if (!report.has_value()) {
      return report_error(context, report.error());
    }
    print_preflight(context, report.value());
    return report.value().passed ? 0 : 3;
  }

  if (options.command == "start") {
    StartOptions start_options;
    start_options.dry_run = options.dry_run;
    start_options.allow_irreversible = options.allow_irreversible;
    start_options.max_parallel = options.parallel;
    start_options.reason = options.reason;
    const Status preflight_status =
        ensure_current_preflight(engine, make_CampaignId(options.campaign), context);
    if (!preflight_status.has_value()) {
      return report_error(context, preflight_status.error());
    }
    const Status started = engine.start(make_CampaignId(options.campaign), start_options);
    if (!started.has_value()) {
      return report_error(context, started.error());
    }
    if (options.dry_run) {
      *context.out << "admission accepted (dry run; nothing was executed)\n";
      return 0;
    }
    const Status ran = engine.run_until_idle();
    if (!ran.has_value()) {
      return report_error(context, ran.error());
    }
    auto status = engine.status(make_CampaignId(options.campaign));
    if (!status.has_value()) {
      return report_error(context, status.error());
    }
    print_status(context, status.value());
    return upgrade_state_is_terminal(status.value().campaign.state) ? 0 : 3;
  }

  if (options.command == "pause" || options.command == "resume" || options.command == "abort") {
    Status result = ok_status();
    if (options.command == "pause") {
      result = engine.pause(make_CampaignId(options.campaign), options.reason);
    } else if (options.command == "resume") {
      const Status preflight_status =
          ensure_current_preflight(engine, make_CampaignId(options.campaign), context);
      if (!preflight_status.has_value()) {
        return report_error(context, preflight_status.error());
      }
      result = engine.resume(make_CampaignId(options.campaign));
    } else {
      result = engine.abort(make_CampaignId(options.campaign), options.reason);
    }
    if (!result.has_value()) {
      return report_error(context, result.error());
    }
    *context.out << options.command << " applied to " << options.campaign << "\n";
    return 0;
  }

  if (options.command == "rollback") {
    auto report = engine.rollback(make_CampaignId(options.campaign), !options.plan_only);
    if (!report.has_value()) {
      return report_error(context, report.error());
    }
    *context.out << (options.plan_only ? "rollback planned: " : "rollback started: ")
                 << report.value().fingerprint << "\n";
    if (!options.plan_only) {
      const Status ran = engine.run_until_idle();
      if (!ran.has_value()) {
        return report_error(context, ran.error());
      }
      auto status = engine.status(make_CampaignId(options.campaign));
      if (!status.has_value()) {
        return report_error(context, status.error());
      }
      print_status(context, status.value());
    }
    return 0;
  }

  if (options.command == "status") {
    if (options.campaign.empty()) {
      auto list = engine.campaigns();
      if (!list.has_value()) {
        return report_error(context, list.error());
      }
      if (context.json) {
        json::Value array = json::Value::make_array();
        for (const auto& status : list.value()) {
          array.push(status.to_json());
        }
        *context.out << array.dump_pretty() << "\n";
        return 0;
      }
      if (list.value().empty()) {
        *context.out << "no campaigns\n";
      }
      for (const auto& status : list.value()) {
        print_status(context, status);
      }
      return 0;
    }
    auto status = engine.status(make_CampaignId(options.campaign));
    if (!status.has_value()) {
      return report_error(context, status.error());
    }
    print_status(context, status.value());
    return 0;
  }

  if (options.command == "explain") {
    auto explanation = engine.explain(make_CampaignId(options.campaign));
    if (!explanation.has_value()) {
      return report_error(context, explanation.error());
    }
    if (context.json) {
      json::Value value = json::Value::make_object();
      value.set("campaign", json::Value::make_string(options.campaign));
      value.set("explanation", json::Value::make_string(explanation.value()));
      auto status = engine.status(make_CampaignId(options.campaign));
      if (status.has_value()) {
        value.set("status", status.value().to_json());
      }
      *context.out << value.dump_pretty() << "\n";
      return 0;
    }
    *context.out << explanation.value();
    return 0;
  }

  if (options.command == "inventory") {
    auto snapshot = engine.inventory();
    if (!snapshot.has_value()) {
      return report_error(context, snapshot.error());
    }
    if (context.json) {
      json::Value value = json::Value::make_object();
      value.set("revision", json::Value::make_uint(snapshot.value().revision.value()));
      value.set("observed_at", json::Value::make_string(snapshot.value().observed_at.to_iso8601()));
      value.set("incarnation", json::Value::make_uint(snapshot.value().incarnation.value()));
      json::Value targets = json::Value::make_array();
      for (const auto& target : snapshot.value().targets) {
        targets.push(target.to_json());
      }
      value.set("targets", std::move(targets));
      *context.out << value.dump_pretty() << "\n";
      return 0;
    }
    *context.out << "inventory revision " << snapshot.value().revision.value() << " observed "
                 << snapshot.value().observed_at.to_iso8601() << "\n";
    for (const auto& target : snapshot.value().targets) {
      *context.out << "  " << target.id.str() << " " << target.component.str() << " version "
                   << target.installed_version.text() << " build " << target.installed_build.str()
                   << " platform " << target.platform << " adapter " << target.adapter.str()
                   << " strategies";
      for (const auto strategy :
           {StrategyKind::in_place, StrategyKind::restart_based,
            StrategyKind::redundant_pair_rolling, StrategyKind::control_plane_generation_handoff}) {
        if (target.claims.supports(strategy)) {
          *context.out << " " << strategy_kind_name(strategy);
        }
      }
      *context.out << "\n";
    }
    return 0;
  }

  if (options.command == "provenance") {
    CampaignId filter;
    if (!options.campaign.empty()) {
      filter = make_CampaignId(options.campaign);
    }
    auto records = engine.provenance(filter);
    if (!records.has_value()) {
      return report_error(context, records.error());
    }
    if (context.json) {
      json::Value array = json::Value::make_array();
      for (const auto& record : records.value()) {
        array.push(record.to_json());
      }
      *context.out << array.dump_pretty() << "\n";
      return 0;
    }
    for (const auto& record : records.value()) {
      *context.out << record.sequence.value() << " " << record.recorded_at.to_iso8601() << " "
                   << provenance_outcome_name(record.outcome) << " campaign "
                   << record.campaign.str() << " gen " << record.generation.value() << " target "
                   << record.target.str() << " " << record.from_version.text() << " -> "
                   << record.to_version.text() << " artifact " << record.artifact.str()
                   << " digest " << record.digest.hex().substr(0, 16) << " authority "
                   << record.authority.str() << "\n";
    }
    return 0;
  }

  if (options.command == "decisions") {
    auto decisions = engine.decisions(make_CampaignId(options.campaign));
    if (!decisions.has_value()) {
      return report_error(context, decisions.error());
    }
    if (context.json) {
      json::Value array = json::Value::make_array();
      for (const auto& decision : decisions.value()) {
        array.push(decision.to_json());
      }
      *context.out << array.dump_pretty() << "\n";
      return 0;
    }
    for (const auto& decision : decisions.value()) {
      *context.out << decision.explain() << "\n";
    }
    return 0;
  }

  if (options.command == "stats") {
    *context.out << engine.stats().dump_pretty() << "\n";
    return 0;
  }

  *context.err << "unknown command: " << options.command << "\n";
  return 2;
}

}  // namespace fum::cli
