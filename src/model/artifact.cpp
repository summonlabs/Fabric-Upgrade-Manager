#include "fum/model/artifact.hpp"

#include <cstdio>

#include "fum/core/checked.hpp"
#include "fum/core/fs.hpp"
#include "fum/core/hash.hpp"

namespace fum {
namespace {

Result<std::string> require_identifier(const json::Value& value, std::string_view key,
                                       std::string_view what) {
  std::string_view text;
  FUM_TRY(text, value.require_string(key));
  FUM_TRYV(validate_identifier(text, what));
  return std::string(text);
}

Result<std::vector<std::string>> require_string_array(const json::Value& value,
                                                      std::string_view key) {
  const json::Value* found = value.find(key);
  std::vector<std::string> out;
  if (found == nullptr) {
    return out;
  }
  if (!found->is_array()) {
    return make_error(ErrorCode::invalid_argument, "json member must be an array",
                      std::string(key));
  }
  for (const auto& item : found->items()) {
    std::string_view text;
    FUM_TRY(text, item.as_string());
    out.emplace_back(text);
  }
  return out;
}

}  // namespace

const char* component_type_name(ComponentType type) noexcept {
  switch (type) {
    case ComponentType::software: return "software";
    case ComponentType::firmware: return "firmware";
    case ComponentType::control_plane: return "control-plane";
    case ComponentType::driver: return "driver";
    case ComponentType::agent: return "agent";
  }
  return "unknown";
}

Result<ComponentType> parse_component_type(std::string_view text) {
  if (text == "software") return ComponentType::software;
  if (text == "firmware") return ComponentType::firmware;
  if (text == "control-plane") return ComponentType::control_plane;
  if (text == "driver") return ComponentType::driver;
  if (text == "agent") return ComponentType::agent;
  return make_error(ErrorCode::invalid_argument, "unknown component type", std::string(text));
}

json::Value SignerProvenance::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("signer", json::Value::make_string(signer.str()));
  value.set("key_id", json::Value::make_string(key_id));
  value.set("algorithm", json::Value::make_string(algorithm));
  value.set("attestation_ref", json::Value::make_string(attestation_ref));
  value.set("source_revision", json::Value::make_string(source_revision));
  value.set("build_system", json::Value::make_string(build_system));
  return value;
}

Result<SignerProvenance> SignerProvenance::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "provenance must be a json object");
  }
  SignerProvenance out;
  std::string_view signer_text;
  FUM_TRY(signer_text, value.require_string("signer"));
  if (!signer_text.empty()) {
    FUM_TRY(out.signer, SignerId::parse(signer_text));
  }
  const json::Value* key_id = value.find("key_id");
  if (key_id != nullptr) {
    std::string_view text;
    FUM_TRY(text, key_id->as_string());
    out.key_id = std::string(text);
  }
  const json::Value* algorithm = value.find("algorithm");
  if (algorithm != nullptr) {
    std::string_view text;
    FUM_TRY(text, algorithm->as_string());
    out.algorithm = std::string(text);
  }
  if (out.declared()) {
    FUM_TRYV(validate_identifier(out.key_id, "key id"));
    if (out.algorithm.empty()) {
      return make_error(ErrorCode::invalid_argument,
                        "a declared signer requires an algorithm");
    }
  }
  const json::Value* attestation = value.find("attestation_ref");
  if (attestation != nullptr) {
    std::string_view text;
    FUM_TRY(text, attestation->as_string());
    out.attestation_ref = std::string(text);
  }
  const json::Value* revision = value.find("source_revision");
  if (revision != nullptr) {
    std::string_view text;
    FUM_TRY(text, revision->as_string());
    out.source_revision = std::string(text);
  }
  const json::Value* build_system = value.find("build_system");
  if (build_system != nullptr) {
    std::string_view text;
    FUM_TRY(text, build_system->as_string());
    out.build_system = std::string(text);
  }
  return out;
}

json::Value CapabilityRequirement::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("name", json::Value::make_string(name));
  value.set("minimum", json::Value::make_string(minimum));
  return value;
}

Result<CapabilityRequirement> CapabilityRequirement::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "capability requirement must be an object");
  }
  CapabilityRequirement out;
  FUM_TRY(out.name, require_identifier(value, "name", "capability name"));
  std::string_view minimum;
  FUM_TRY(minimum, value.require_string("minimum"));
  out.minimum = std::string(minimum);
  return out;
}

json::Value RollbackCompatibility::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("reversible", json::Value::make_bool(reversible));
  value.set("rollback_artifact", json::Value::make_string(rollback_artifact.str()));
  value.set("boundary", json::Value::make_string(boundary));
  json::Value steps = json::Value::make_array();
  for (const auto& step : irreversible_steps) {
    steps.push(json::Value::make_string(step.str()));
  }
  value.set("irreversible_steps", std::move(steps));
  value.set("notes", json::Value::make_string(notes));
  return value;
}

Result<RollbackCompatibility> RollbackCompatibility::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "rollback metadata must be an object");
  }
  RollbackCompatibility out;
  FUM_TRY(out.reversible, value.require_bool("reversible"));
  const json::Value* artifact = value.find("rollback_artifact");
  if (artifact != nullptr) {
    std::string_view text;
    FUM_TRY(text, artifact->as_string());
    if (!text.empty()) {
      FUM_TRY(out.rollback_artifact, ArtifactId::parse(text));
    }
  }
  const json::Value* boundary = value.find("boundary");
  if (boundary != nullptr) {
    std::string_view text;
    FUM_TRY(text, boundary->as_string());
    out.boundary = std::string(text);
  }
  const json::Value* irreversible = value.find("irreversible_steps");
  if (irreversible != nullptr && !irreversible->is_null()) {
    if (!irreversible->is_array()) {
      return make_error(ErrorCode::invalid_argument, "irreversible_steps must be an array");
    }
    for (const auto& item : irreversible->items()) {
      std::string_view step_text;
      FUM_TRY(step_text, item.as_string());
      StepId step;
      FUM_TRY(step, StepId::parse(step_text));
      out.irreversible_steps.push_back(std::move(step));
    }
  }
  const json::Value* notes = value.find("notes");
  if (notes != nullptr) {
    std::string_view text;
    FUM_TRY(text, notes->as_string());
    out.notes = std::string(text);
  }
  if (!out.reversible && out.boundary.empty()) {
    return make_error(ErrorCode::invalid_argument,
                      "an artifact marked irreversible must declare its boundary");
  }
  return out;
}

json::Value SchemaProtocolVersions::to_json() const {
  json::Value value = json::Value::make_object();
  json::Value schema = json::Value::make_array();
  for (const auto& item : schema_versions) {
    schema.push(json::Value::make_string(item));
  }
  json::Value protocol = json::Value::make_array();
  for (const auto& item : protocol_versions) {
    protocol.push(json::Value::make_string(item));
  }
  value.set("schema_versions", std::move(schema));
  value.set("protocol_versions", std::move(protocol));
  return value;
}

Result<SchemaProtocolVersions> SchemaProtocolVersions::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "schema/protocol metadata must be an object");
  }
  SchemaProtocolVersions out;
  FUM_TRY(out.schema_versions, require_string_array(value, "schema_versions"));
  FUM_TRY(out.protocol_versions, require_string_array(value, "protocol_versions"));
  return out;
}

json::Value ArtifactDescriptor::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("artifact_id", json::Value::make_string(id_.str()));
  value.set("component_id", json::Value::make_string(component_.str()));
  value.set("version", json::Value::make_string(version_.text()));
  value.set("build_id", json::Value::make_string(build_.str()));
  value.set("component_type", json::Value::make_string(component_type_name(component_type_)));
  value.set("digest", json::Value::make_string(digest_.hex()));
  value.set("size_bytes", json::Value::make_uint(size_bytes_));
  if (provenance_.declared()) {
    value.set("provenance", provenance_.to_json());
  }
  json::Value platforms = json::Value::make_array();
  for (const auto& platform : platforms_) {
    platforms.push(json::Value::make_string(platform));
  }
  value.set("platforms", std::move(platforms));
  json::Value capabilities = json::Value::make_array();
  for (const auto& capability : capabilities_) {
    capabilities.push(capability.to_json());
  }
  value.set("capabilities", std::move(capabilities));
  value.set("versions", versions_.to_json());
  value.set("rollback", rollback_.to_json());
  value.set("created_at", json::Value::make_string(created_at_.to_iso8601()));
  value.set("media_type", json::Value::make_string(media_type_));
  return value;
}

Result<ArtifactDescriptor> ArtifactDescriptor::from_json(const json::Value& value) {
  if (!value.is_object()) {
    return make_error(ErrorCode::invalid_argument, "artifact descriptor must be a json object");
  }
  ArtifactDescriptor out;
  std::string text;
  FUM_TRY(text, require_identifier(value, "artifact_id", "artifact id"));
  FUM_TRY(out.id_, ArtifactId::parse(text));
  FUM_TRY(text, require_identifier(value, "component_id", "component id"));
  FUM_TRY(out.component_, ComponentId::parse(text));
  std::string_view version_text;
  FUM_TRY(version_text, value.require_string("version"));
  FUM_TRY(out.version_, Version::parse(version_text));
  FUM_TRY(text, require_identifier(value, "build_id", "build id"));
  FUM_TRY(out.build_, BuildId::parse(text));
  std::string_view type_text;
  FUM_TRY(type_text, value.require_string("component_type"));
  FUM_TRY(out.component_type_, parse_component_type(type_text));
  std::string_view digest_text;
  FUM_TRY(digest_text, value.require_string("digest"));
  FUM_TRY(out.digest_, Digest::parse(digest_text));
  FUM_TRY(out.size_bytes_, value.require_uint("size_bytes"));
  const json::Value* provenance = value.find("provenance");
  if (provenance != nullptr) {
    FUM_TRY(out.provenance_, SignerProvenance::from_json(*provenance));
  }
  FUM_TRY(out.platforms_, require_string_array(value, "platforms"));
  const json::Value* capabilities = value.find("capabilities");
  if (capabilities != nullptr) {
    if (!capabilities->is_array()) {
      return make_error(ErrorCode::invalid_argument, "capabilities must be an array");
    }
    for (const auto& item : capabilities->items()) {
      CapabilityRequirement requirement;
      FUM_TRY(requirement, CapabilityRequirement::from_json(item));
      out.capabilities_.push_back(std::move(requirement));
    }
  }
  const json::Value* versions = value.find("versions");
  if (versions != nullptr) {
    FUM_TRY(out.versions_, SchemaProtocolVersions::from_json(*versions));
  }
  const json::Value* rollback = value.find("rollback");
  if (rollback != nullptr) {
    FUM_TRY(out.rollback_, RollbackCompatibility::from_json(*rollback));
  }
  const json::Value* created = value.find("created_at");
  if (created != nullptr) {
    std::string_view created_text;
    FUM_TRY(created_text, created->as_string());
    FUM_TRY(out.created_at_, Timestamp::parse_iso8601(created_text));
  }
  const json::Value* media = value.find("media_type");
  if (media != nullptr) {
    std::string_view media_text;
    FUM_TRY(media_text, media->as_string());
    out.media_type_ = std::string(media_text);
  }
  FUM_TRYV(out.validate());
  return out;
}

bool ArtifactDescriptor::supported_on(std::string_view platform) const {
  if (platforms_.empty()) {
    return false;  // an artifact with no declared platform is never assumed portable
  }
  for (const auto& declared : platforms_) {
    if (declared == platform) {
      return true;
    }
  }
  return false;
}

Status ArtifactDescriptor::validate() const {
  if (id_.empty()) {
    return make_error(ErrorCode::invalid_argument, "artifact id is required");
  }
  if (component_.empty()) {
    return make_error(ErrorCode::invalid_argument, "artifact component id is required");
  }
  if (build_.empty()) {
    return make_error(ErrorCode::invalid_argument, "artifact build id is required");
  }
  if (version_.empty()) {
    return make_error(ErrorCode::invalid_argument, "artifact version is required");
  }
  if (digest_.empty()) {
    return make_error(ErrorCode::invalid_argument, "artifact digest is required");
  }
  FUM_TRYV(checked::require_bound(size_bytes_, kMaxArtifactBytes, "artifact size"));
  if (size_bytes_ == 0) {
    return make_error(ErrorCode::invalid_argument, "artifact size must be greater than zero");
  }
  if (platforms_.empty()) {
    return make_error(ErrorCode::invalid_argument,
                      "artifact must declare at least one supported platform");
  }
  if (provenance_.declared()) {
    if (provenance_.key_id.empty() || provenance_.algorithm.empty()) {
      return make_error(ErrorCode::invalid_argument,
                        "a declared signer requires a key id and an algorithm");
    }
  }
  if (!rollback_.reversible && rollback_.boundary.empty()) {
    return make_error(ErrorCode::invalid_argument,
                      "an artifact that is not reversible must declare its irreversible boundary");
  }
  return ok_status();
}

json::Value IntegrityAssessment::to_json() const {
  json::Value value = json::Value::make_object();
  value.set("verified", json::Value::make_bool(verified));
  value.set("observed_digest", json::Value::make_string(observed.hex()));
  value.set("declared_digest", json::Value::make_string(declared.hex()));
  value.set("bytes", json::Value::make_uint(bytes));
  value.set("detail", json::Value::make_string(detail));
  return value;
}

IntegrityAssessment verify_artifact_bytes(const ArtifactDescriptor& descriptor,
                                          std::string_view payload) {
  IntegrityAssessment assessment;
  assessment.declared = descriptor.digest();
  assessment.observed = Digest::of_bytes(payload);
  assessment.bytes = static_cast<std::uint64_t>(payload.size());
  assessment.verified = assessment.observed == assessment.declared &&
                        assessment.bytes == descriptor.size_bytes();
  if (assessment.verified) {
    assessment.detail = "digest and size match the descriptor";
  } else if (assessment.observed != assessment.declared) {
    assessment.detail = "digest mismatch: declared " + assessment.declared.hex() + ", observed " +
                        assessment.observed.hex();
  } else {
    assessment.detail = "size mismatch: declared " + std::to_string(descriptor.size_bytes()) +
                        ", observed " + std::to_string(assessment.bytes);
  }
  return assessment;
}

IntegrityAssessment verify_artifact_file(const ArtifactDescriptor& descriptor,
                                         const std::string& path) {
  IntegrityAssessment assessment;
  assessment.declared = descriptor.digest();
  auto size = fs::file_size(path);
  if (!size.has_value()) {
    assessment.detail = size.error().to_string();
    return assessment;
  }
  assessment.bytes = size.value();
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path.c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "rb");
#endif
  if (file == nullptr) {
    assessment.detail = "could not open artifact payload: " + path;
    return assessment;
  }
  hash::Sha256Stream stream;
  std::string buffer;
  buffer.resize(64 * 1024);
  std::uint64_t total = 0;
  for (;;) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
    if (read > 0) {
      stream.update(std::string_view(buffer.data(), read));
      total += static_cast<std::uint64_t>(read);
    }
    if (read < buffer.size()) {
      break;
    }
  }
  std::fclose(file);
  assessment.observed = Digest::parse(hash::to_hex(stream.finish())).value_or(Digest{});
  assessment.bytes = total;
  assessment.verified = assessment.observed == assessment.declared &&
                        total == descriptor.size_bytes();
  if (assessment.verified) {
    assessment.detail = "digest and size match the descriptor";
  } else {
    assessment.detail = "payload on disk does not match the descriptor digest";
  }
  return assessment;
}

}  // namespace fum
