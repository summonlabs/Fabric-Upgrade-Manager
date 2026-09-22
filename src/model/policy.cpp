#include "fum/model/policy.hpp"

#include "fum/core/hash.hpp"

namespace fum {

Status Policy::validate() const {
  if (id.empty()) {
    return make_error(ErrorCode::invalid_argument, "policy id is required");
  }
  if (max_attempts_per_target == 0 || max_attempts_per_target > 64) {
    return make_error(ErrorCode::invalid_argument, "max_attempts_per_target is out of range");
  }
  if (max_retries_per_operation > 16) {
    return make_error(ErrorCode::invalid_argument, "max_retries_per_operation is out of range");
  }
  if (max_stages == 0 || max_stages > 4096) {
    return make_error(ErrorCode::invalid_argument, "max_stages is out of range");
  }
  if (max_targets == 0 || max_targets > 100000) {
    return make_error(ErrorCode::invalid_argument, "max_targets is out of range");
  }
  if (max_concurrent_activations == 0 || max_concurrent_activations > 256) {
    return make_error(ErrorCode::invalid_argument, "max_concurrent_activations is out of range");
  }
  if (max_decisions_per_campaign == 0 || max_decisions_per_campaign > 100000) {
    return make_error(ErrorCode::invalid_argument, "max_decisions_per_campaign is out of range");
  }
  if (max_operations_per_attempt == 0 || max_operations_per_attempt > 1024) {
    return make_error(ErrorCode::invalid_argument, "max_operations_per_attempt is out of range");
  }
  if (max_provenance_records == 0 || max_provenance_records > 10000000) {
    return make_error(ErrorCode::invalid_argument, "max_provenance_records is out of range");
  }
  for (const auto* window :
       {&inventory_validity, &artifact_validity, &compatibility_validity, &health_validity,
        &verification_validity, &preflight_validity, &attempt_lease}) {
    if (window->is_zero() || window->is_negative()) {
      return make_error(ErrorCode::invalid_argument,
                        "every validity window must be a positive duration");
    }
  }
  FUM_TRYV(default_skew_budget.validate());
  return ok_status();
}

json::Value Policy::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("policy_id", json::Value::make_string(id.str()));
  value.set("revision", json::Value::make_uint(revision.value()));
  value.set("name", json::Value::make_string(name));
  value.set("inventory_validity_ms", json::Value::make_int(inventory_validity.nanos() / 1000000));
  value.set("artifact_validity_ms", json::Value::make_int(artifact_validity.nanos() / 1000000));
  value.set("compatibility_validity_ms",
            json::Value::make_int(compatibility_validity.nanos() / 1000000));
  value.set("health_validity_ms", json::Value::make_int(health_validity.nanos() / 1000000));
  value.set("verification_validity_ms",
            json::Value::make_int(verification_validity.nanos() / 1000000));
  value.set("preflight_validity_ms", json::Value::make_int(preflight_validity.nanos() / 1000000));
  value.set("attempt_lease_ms", json::Value::make_int(attempt_lease.nanos() / 1000000));
  value.set("max_attempts_per_target", json::Value::make_uint(max_attempts_per_target));
  value.set("max_retries_per_operation", json::Value::make_uint(max_retries_per_operation));
  value.set("max_stages", json::Value::make_uint(max_stages));
  value.set("max_targets", json::Value::make_uint(max_targets));
  value.set("max_concurrent_activations", json::Value::make_uint(max_concurrent_activations));
  value.set("max_decisions_per_campaign", json::Value::make_uint(max_decisions_per_campaign));
  value.set("max_operations_per_attempt", json::Value::make_uint(max_operations_per_attempt));
  value.set("max_provenance_records", json::Value::make_uint(max_provenance_records));
  value.set("require_signature_declaration", json::Value::make_bool(require_signature_declaration));
  value.set("require_health_gate", json::Value::make_bool(require_health_gate));
  value.set("allow_irreversible_acknowledgement",
            json::Value::make_bool(allow_irreversible_acknowledgement));
  value.set("stop_on_first_stage_failure", json::Value::make_bool(stop_on_first_stage_failure));
  value.set("default_skew_budget", default_skew_budget.to_json());
  return value;
}

Result<Policy> Policy::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "policy must be a json object");
  }
  Policy out;
  std::string text;
  FUM_TRY(text, value.require_string("policy_id"));
  FUM_TRY(out.id, PolicyId::parse(text));
  std::uint64_t numeric = 0;
  FUM_TRY(numeric, value.require_uint("revision"));
  out.revision = Revision(numeric);
  const json::Value* name = value.find("name");
  if (name != nullptr) {
    FUM_TRY(out.name, name->as_string());
  }
  struct DurationField {
    const char* key;
    Duration* target;
  };
  const DurationField fields[] = {
      {"inventory_validity_ms", &out.inventory_validity},
      {"artifact_validity_ms", &out.artifact_validity},
      {"compatibility_validity_ms", &out.compatibility_validity},
      {"health_validity_ms", &out.health_validity},
      {"verification_validity_ms", &out.verification_validity},
      {"preflight_validity_ms", &out.preflight_validity},
      {"attempt_lease_ms", &out.attempt_lease},
  };
  for (const auto& entry : fields) {
    const json::Value* item = value.find(entry.key);
    if (item == nullptr) {
      continue;
    }
    std::int64_t millis = 0;
    FUM_TRY(millis, item->as_int());
    if (millis <= 0) {
      return make_error(ErrorCode::invalid_argument, "policy duration must be positive",
                        std::string(entry.key));
    }
    *entry.target = Duration::from_millis(millis);
  }
  struct CountField {
    const char* key;
    std::uint32_t* target;
  };
  const CountField counts[] = {
      {"max_attempts_per_target", &out.max_attempts_per_target},
      {"max_retries_per_operation", &out.max_retries_per_operation},
      {"max_stages", &out.max_stages},
      {"max_targets", &out.max_targets},
      {"max_concurrent_activations", &out.max_concurrent_activations},
      {"max_decisions_per_campaign", &out.max_decisions_per_campaign},
      {"max_operations_per_attempt", &out.max_operations_per_attempt},
  };
  for (const auto& entry : counts) {
    const json::Value* item = value.find(entry.key);
    if (item == nullptr) {
      continue;
    }
    FUM_TRY(numeric, item->as_uint());
    if (numeric > 0xFFFFFFFFull) {
      return make_error(ErrorCode::invalid_argument, "policy count is out of range",
                        std::string(entry.key));
    }
    *entry.target = static_cast<std::uint32_t>(numeric);
  }
  const json::Value* provenance = value.find("max_provenance_records");
  if (provenance != nullptr) {
    FUM_TRY(numeric, provenance->as_uint());
    if (numeric > 0xFFFFFFFFull) {
      return make_error(ErrorCode::invalid_argument, "max_provenance_records is out of range");
    }
    out.max_provenance_records = static_cast<std::uint32_t>(numeric);
  }
  struct BoolField {
    const char* key;
    bool* target;
  };
  const BoolField bools[] = {
      {"require_signature_declaration", &out.require_signature_declaration},
      {"require_health_gate", &out.require_health_gate},
      {"allow_irreversible_acknowledgement", &out.allow_irreversible_acknowledgement},
      {"stop_on_first_stage_failure", &out.stop_on_first_stage_failure},
  };
  for (const auto& entry : bools) {
    const json::Value* item = value.find(entry.key);
    if (item == nullptr) {
      continue;
    }
    FUM_TRY(*entry.target, item->as_bool());
  }
  const json::Value* budget = value.find("default_skew_budget");
  if (budget != nullptr) {
    FUM_TRY(out.default_skew_budget, SkewBudget::from_json(*budget));
  }
  FUM_TRYV(out.validate());
  return out;
}

std::string Policy::fingerprint() const { return hash::sha256_hex(to_json().dump()); }

}  // namespace fum
