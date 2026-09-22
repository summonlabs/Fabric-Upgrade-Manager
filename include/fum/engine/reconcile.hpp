// Restart reconciliation: no in-flight work survives a process incarnation.
#pragma once

#include <string>
#include <vector>

#include "fum/model/campaign.hpp"

namespace fum {

enum class ReconcileActionKind : std::uint8_t {
  abandon_attempt = 0,
  block_campaign,
  invalidate_ticket,
  note,
};

[[nodiscard]] const char* reconcile_action_kind_name(ReconcileActionKind kind) noexcept;

struct [[nodiscard]] ReconcileAction {
  ReconcileActionKind kind = ReconcileActionKind::note;
  std::string subject;
  std::string reason;

  [[nodiscard]] json::Value to_json() const;
};

struct [[nodiscard]] ReconcileReport {
  std::vector<ReconcileAction> actions;
  std::size_t abandoned_attempts = 0;
  std::size_t blocked_campaigns = 0;
  std::size_t invalidated_tickets = 0;
  Incarnation incarnation;

  [[nodiscard]] json::Value to_json() const;
};

// Pure function: decides what must change after a restart. The caller applies
// the actions durably.
[[nodiscard]] ReconcileReport plan_reconciliation(const std::vector<CampaignRecord>& campaigns,
                                                  const std::vector<AttemptRecord>& attempts,
                                                  Incarnation current, Timestamp now);

}  // namespace fum
