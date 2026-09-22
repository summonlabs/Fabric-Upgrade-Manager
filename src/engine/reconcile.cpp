#include "fum/engine/reconcile.hpp"

#include <algorithm>

namespace fum {

const char* reconcile_action_kind_name(ReconcileActionKind kind) noexcept {
  switch (kind) {
    case ReconcileActionKind::abandon_attempt: return "abandon-attempt";
    case ReconcileActionKind::block_campaign: return "block-campaign";
    case ReconcileActionKind::invalidate_ticket: return "invalidate-ticket";
    case ReconcileActionKind::note: return "note";
  }
  return "unknown";
}

json::Value ReconcileAction::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("kind", json::Value::make_string(reconcile_action_kind_name(kind)));
  value.set("subject", json::Value::make_string(subject));
  value.set("reason", json::Value::make_string(reason));
  return value;
}

json::Value ReconcileReport::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("abandoned_attempts", json::Value::make_uint(abandoned_attempts));
  value.set("blocked_campaigns", json::Value::make_uint(blocked_campaigns));
  value.set("invalidated_tickets", json::Value::make_uint(invalidated_tickets));
  value.set("incarnation", json::Value::make_uint(incarnation.value()));
  json::Value actions_json = json::Value::make_array();
  for (const auto& action : actions) {
    actions_json.push(action.to_json());
  }
  value.set("actions", std::move(actions_json));
  return value;
}

ReconcileReport plan_reconciliation(const std::vector<CampaignRecord>& campaigns,
                                    const std::vector<AttemptRecord>& attempts,
                                    Incarnation current, Timestamp now) {
  ReconcileReport report;
  report.incarnation = current;
  static_cast<void>(now);

  for (const auto& attempt : attempts) {
    if (!attempt_state_is_in_flight(attempt.state)) {
      continue;
    }
    // Work owned by a previous incarnation is abandoned: it cannot be resumed
    // because the in-memory context that produced it is gone.
    ReconcileAction action;
    action.kind = ReconcileActionKind::abandon_attempt;
    action.subject = attempt.id.str();
    action.reason = attempt.owner == current
                        ? "attempt is in flight in the current incarnation"
                        : "attempt was owned by incarnation " +
                              std::to_string(attempt.owner.value()) +
                              " and cannot be resumed by incarnation " +
                              std::to_string(current.value());
    if (attempt.owner != current) {
      report.actions.push_back(std::move(action));
      report.abandoned_attempts += 1;
    }
  }

  for (const auto& campaign : campaigns) {
    if (campaign.state == UpgradeState::activating || campaign.state == UpgradeState::verifying ||
        campaign.state == UpgradeState::rolling_back) {
      ReconcileAction action;
      action.kind = ReconcileActionKind::block_campaign;
      action.subject = campaign.id.str();
      action.reason = std::string("campaign was ") + upgrade_state_name(campaign.state) +
                      " when incarnation " + std::to_string(campaign.owner_incarnation.value()) +
                      " stopped; it is blocked until an operator resumes it under a fresh "
                      "preflight";
      report.actions.push_back(std::move(action));
      report.blocked_campaigns += 1;
    }
    if (campaign.ticket.issued_at.unix_nanos() != 0 &&
        (campaign.ticket.incarnation != current || campaign.ticket.expires_at.before(now))) {
      ReconcileAction action;
      action.kind = ReconcileActionKind::invalidate_ticket;
      action.subject = campaign.id.str();
      action.reason = campaign.ticket.incarnation != current
                          ? "preflight ticket was issued in incarnation " +
                                std::to_string(campaign.ticket.incarnation.value())
                          : "preflight ticket expired while the runtime was not running";
      report.actions.push_back(std::move(action));
      report.invalidated_tickets += 1;
    }
  }
  return report;
}

}  // namespace fum
