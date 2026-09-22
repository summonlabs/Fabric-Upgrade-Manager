#include "fum/model/decision.hpp"

#include "fum/core/hash.hpp"

namespace fum {

const char* decision_kind_name(DecisionKind kind) noexcept {
  switch (kind) {
    case DecisionKind::plan: return "plan";
    case DecisionKind::preflight: return "preflight";
    case DecisionKind::admit: return "admit";
    case DecisionKind::stage: return "stage";
    case DecisionKind::activate: return "activate";
    case DecisionKind::verify: return "verify";
    case DecisionKind::block: return "block";
    case DecisionKind::pause: return "pause";
    case DecisionKind::resume: return "resume";
    case DecisionKind::abort: return "abort";
    case DecisionKind::rollback: return "rollback";
    case DecisionKind::reconcile: return "reconcile";
    case DecisionKind::fence: return "fence";
  }
  return "unknown";
}

Result<DecisionKind> parse_decision_kind(std::string_view text) {
  for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(DecisionKind::fence); ++i) {
    const auto kind = static_cast<DecisionKind>(i);
    if (text == decision_kind_name(kind)) {
      return kind;
    }
  }
  return make_error(ErrorCode::invalid_argument, "unknown decision kind", std::string(text));
}

const char* decision_outcome_name(DecisionOutcome outcome) noexcept {
  switch (outcome) {
    case DecisionOutcome::allow: return "allow";
    case DecisionOutcome::deny: return "deny";
    case DecisionOutcome::defer: return "defer";
  }
  return "unknown";
}

Result<DecisionOutcome> parse_decision_outcome(std::string_view text) {
  if (text == "allow") return DecisionOutcome::allow;
  if (text == "deny") return DecisionOutcome::deny;
  if (text == "defer") return DecisionOutcome::defer;
  return make_error(ErrorCode::invalid_argument, "unknown decision outcome", std::string(text));
}

std::string Decision::fingerprint() const {
  json::Value material = json::Value::make_object();
  material.set("kind", json::Value::make_string(decision_kind_name(kind)));
  material.set("outcome", json::Value::make_string(decision_outcome_name(outcome)));
  material.set("campaign", json::Value::make_string(campaign.str()));
  material.set("generation", json::Value::make_uint(generation.value()));
  material.set("authority", json::Value::make_string(authority.str()));
  material.set("epoch", json::Value::make_uint(epoch.value()));
  material.set("policy", json::Value::make_string(policy.str()));
  material.set("policy_revision", json::Value::make_uint(policy_revision.value()));
  json::Value inputs_json = json::Value::make_array();
  for (const auto& input : inputs) {
    json::Value entry = json::Value::make_object();
    entry.set("name", json::Value::make_string(input.name));
    entry.set("value", json::Value::make_string(input.value));
    inputs_json.push(std::move(entry));
  }
  material.set("inputs", std::move(inputs_json));
  json::Value evidence_json = json::Value::make_array();
  for (const auto& item : this->evidence) {
    json::Value entry = json::Value::make_object();
    entry.set("id", json::Value::make_string(item.id.str()));
    entry.set("source", json::Value::make_string(item.source));
    entry.set("summary", json::Value::make_string(item.summary));
    evidence_json.push(std::move(entry));
  }
  material.set("evidence", std::move(evidence_json));
  material.set("selected", json::Value::make_string(selected));
  json::Value rejected_json = json::Value::make_array();
  for (const auto& item : this->rejected) {
    json::Value entry = json::Value::make_object();
    entry.set("action", json::Value::make_string(item.action));
    entry.set("reason", json::Value::make_string(item.reason));
    rejected_json.push(std::move(entry));
  }
  material.set("rejected", std::move(rejected_json));
  material.set("rationale", json::Value::make_string(rationale));
  return hash::sha256_hex(material.dump());
}

json::Value Decision::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("decision_id", json::Value::make_string(id.str()));
  value.set("kind", json::Value::make_string(decision_kind_name(kind)));
  value.set("outcome", json::Value::make_string(decision_outcome_name(outcome)));
  value.set("campaign", json::Value::make_string(campaign.str()));
  value.set("generation", json::Value::make_uint(generation.value()));
  value.set("authority", json::Value::make_string(authority.str()));
  value.set("incarnation", json::Value::make_uint(incarnation.value()));
  value.set("epoch", json::Value::make_uint(epoch.value()));
  value.set("policy", json::Value::make_string(policy.str()));
  value.set("policy_revision", json::Value::make_uint(policy_revision.value()));
  json::Value inputs_json = json::Value::make_array();
  for (const auto& input : inputs) {
    json::Value entry = json::Value::make_object();
    entry.set("name", json::Value::make_string(input.name));
    entry.set("value", json::Value::make_string(input.value));
    inputs_json.push(std::move(entry));
  }
  value.set("inputs", std::move(inputs_json));
  json::Value evidence_json = json::Value::make_array();
  for (const auto& item : this->evidence) {
    json::Value entry = json::Value::make_object();
    entry.set("evidence_id", json::Value::make_string(item.id.str()));
    entry.set("source", json::Value::make_string(item.source));
    entry.set("summary", json::Value::make_string(item.summary));
    evidence_json.push(std::move(entry));
  }
  value.set("evidence", std::move(evidence_json));
  value.set("selected", json::Value::make_string(selected));
  json::Value rejected_json = json::Value::make_array();
  for (const auto& item : this->rejected) {
    json::Value entry = json::Value::make_object();
    entry.set("action", json::Value::make_string(item.action));
    entry.set("reason", json::Value::make_string(item.reason));
    rejected_json.push(std::move(entry));
  }
  value.set("rejected", std::move(rejected_json));
  value.set("rationale", json::Value::make_string(rationale));
  value.set("at", json::Value::make_string(at.to_iso8601()));
  value.set("fingerprint", json::Value::make_string(fingerprint()));
  return value;
}

Result<Decision> Decision::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "decision must be a json object");
  }
  Decision out;
  std::string text;
  FUM_TRY(text, value.require_string("decision_id"));
  FUM_TRY(out.id, DecisionId::parse(text));
  std::string_view kind_text;
  FUM_TRY(kind_text, value.require_string("kind"));
  FUM_TRY(out.kind, parse_decision_kind(kind_text));
  std::string_view outcome_text;
  FUM_TRY(outcome_text, value.require_string("outcome"));
  FUM_TRY(out.outcome, parse_decision_outcome(outcome_text));
  FUM_TRY(text, value.require_string("campaign"));
  FUM_TRY(out.campaign, CampaignId::parse(text));
  std::uint64_t numeric = 0;
  FUM_TRY(numeric, value.require_uint("generation"));
  out.generation = Generation(numeric);
  FUM_TRY(text, value.require_string("authority"));
  FUM_TRY(out.authority, AuthorityId::parse(text));
  const json::Value* incarnation = value.find("incarnation");
  if (incarnation != nullptr) {
    FUM_TRY(numeric, incarnation->as_uint());
    out.incarnation = Incarnation(numeric);
  }
  const json::Value* epoch = value.find("epoch");
  if (epoch != nullptr) {
    FUM_TRY(numeric, epoch->as_uint());
    out.epoch = Epoch(numeric);
  }
  FUM_TRY(text, value.require_string("policy"));
  FUM_TRY(out.policy, PolicyId::parse(text));
  FUM_TRY(numeric, value.require_uint("policy_revision"));
  out.policy_revision = Revision(numeric);
  const json::Value* inputs = value.find("inputs");
  if (inputs != nullptr && inputs->is_array()) {
    for (const auto& item : inputs->items()) {
      DecisionInput input;
      FUM_TRY(input.name, item.require_string("name"));
      FUM_TRY(input.value, item.require_string("value"));
      out.inputs.push_back(std::move(input));
    }
  }
  const json::Value* evidence = value.find("evidence");
  if (evidence != nullptr && evidence->is_array()) {
    for (const auto& item : evidence->items()) {
      DecisionEvidenceRef ref;
      std::string_view id_text;
      FUM_TRY(id_text, item.require_string("evidence_id"));
      FUM_TRY(ref.id, EvidenceId::parse(id_text));
      FUM_TRY(ref.source, item.require_string("source"));
      FUM_TRY(ref.summary, item.require_string("summary"));
      out.evidence.push_back(std::move(ref));
    }
  }
  FUM_TRY(out.selected, value.require_string("selected"));
  const json::Value* rejected = value.find("rejected");
  if (rejected != nullptr && rejected->is_array()) {
    for (const auto& item : rejected->items()) {
      RejectedAlternative alternative;
      FUM_TRY(alternative.action, item.require_string("action"));
      FUM_TRY(alternative.reason, item.require_string("reason"));
      out.rejected.push_back(std::move(alternative));
    }
  }
  FUM_TRY(out.rationale, value.require_string("rationale"));
  const json::Value* at = value.find("at");
  if (at != nullptr) {
    std::string_view at_text;
    FUM_TRY(at_text, at->as_string());
    FUM_TRY(out.at, Timestamp::parse_iso8601(at_text));
  }
  return out;
}

std::string Decision::explain() const {
  std::string out;
  out.append(decision_kind_name(kind));
  out.append(" decision ");
  out.append(decision_outcome_name(outcome));
  out.append(": ");
  out.append(selected);
  out.append("\n  rationale: ");
  out.append(rationale);
  out.append("\n  campaign=");
  out.append(campaign.str());
  out.append(" generation=");
  out.append(std::to_string(generation.value()));
  out.append(" authority=");
  out.append(authority.str());
  out.append(" epoch=");
  out.append(std::to_string(epoch.value()));
  out.append(" policy=");
  out.append(policy.str());
  out.append("@");
  out.append(std::to_string(policy_revision.value()));
  out.append("\n  inputs:");
  for (const auto& input : inputs) {
    out.append("\n    ");
    out.append(input.name);
    out.append("=");
    out.append(input.value);
  }
  if (!evidence.empty()) {
    out.append("\n  evidence:");
    for (const auto& ref : evidence) {
      out.append("\n    ");
      out.append(ref.source);
      out.append(" ");
      out.append(ref.id.str());
      out.append(": ");
      out.append(ref.summary);
    }
  }
  if (!rejected.empty()) {
    out.append("\n  rejected alternatives:");
    for (const auto& alternative : rejected) {
      out.append("\n    ");
      out.append(alternative.action);
      out.append(": ");
      out.append(alternative.reason);
    }
  }
  out.append("\n  fingerprint: ");
  out.append(fingerprint());
  return out;
}

}  // namespace fum
