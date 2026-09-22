// Artifact identity, integrity and provenance metadata.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/result.hpp"
#include "fum/core/time.hpp"
#include "fum/model/version.hpp"

namespace fum {

enum class ComponentType : std::uint8_t {
  software,
  firmware,
  control_plane,
  driver,
  agent,
};

[[nodiscard]] const char* component_type_name(ComponentType type) noexcept;
[[nodiscard]] Result<ComponentType> parse_component_type(std::string_view text);

// Provenance hooks. Fabric Upgrade Manager records and enforces the declared
// hooks; it does not implement a signing authority of its own.
struct [[nodiscard]] SignerProvenance {
  SignerId signer;
  std::string key_id;
  std::string algorithm;         // declared signature algorithm, e.g. "ed25519"
  std::string attestation_ref;   // external attestation service reference
  std::string source_revision;   // source revision the artifact was built from
  std::string build_system;

  [[nodiscard]] bool declared() const noexcept { return !signer.empty(); }
  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<SignerProvenance> from_json(const json::Value& value);
};

struct [[nodiscard]] CapabilityRequirement {
  std::string name;
  std::string minimum;  // opaque, comparable by the target adapter

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<CapabilityRequirement> from_json(const json::Value& value);
};

// Irreversible boundaries: explicitly declared before execution, never inferred.
struct [[nodiscard]] RollbackCompatibility {
  bool reversible = false;
  ArtifactId rollback_artifact;              // known-good artifact to return to
  std::string boundary;                      // the boundary that cannot be crossed back
  std::vector<StepId> irreversible_steps;    // explicit irreversible step identities
  std::string notes;

  [[nodiscard]] bool has_irreversible_steps() const noexcept {
    return !irreversible_steps.empty() || !reversible;
  }
  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<RollbackCompatibility> from_json(const json::Value& value);
};

struct [[nodiscard]] SchemaProtocolVersions {
  std::vector<std::string> schema_versions;
  std::vector<std::string> protocol_versions;

  [[nodiscard]] json::Value to_json() const;
  [[nodiscard]] static Result<SchemaProtocolVersions> from_json(const json::Value& value);
};

inline constexpr std::uint64_t kMaxArtifactBytes = 8ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxArtifactMetadataBytes = 1ull * 1024ull * 1024ull;

class [[nodiscard]] ArtifactDescriptor {
 public:
  ArtifactDescriptor() = default;

  [[nodiscard]] static Result<ArtifactDescriptor> from_json(const json::Value& value);
  [[nodiscard]] json::Value to_json() const;

  [[nodiscard]] const ArtifactId& id() const noexcept { return id_; }
  [[nodiscard]] const ComponentId& component() const noexcept { return component_; }
  [[nodiscard]] const Version& version() const noexcept { return version_; }
  [[nodiscard]] const BuildId& build() const noexcept { return build_; }
  [[nodiscard]] ComponentType component_type() const noexcept { return component_type_; }
  [[nodiscard]] const Digest& digest() const noexcept { return digest_; }
  [[nodiscard]] std::uint64_t size_bytes() const noexcept { return size_bytes_; }
  [[nodiscard]] const SignerProvenance& provenance() const noexcept { return provenance_; }
  [[nodiscard]] const std::vector<std::string>& platforms() const noexcept { return platforms_; }
  [[nodiscard]] const std::vector<CapabilityRequirement>& capabilities() const noexcept {
    return capabilities_;
  }
  [[nodiscard]] const SchemaProtocolVersions& versions() const noexcept { return versions_; }
  [[nodiscard]] const RollbackCompatibility& rollback() const noexcept { return rollback_; }
  [[nodiscard]] Timestamp created_at() const noexcept { return created_at_; }
  [[nodiscard]] const std::string& media_type() const noexcept { return media_type_; }

  void set_id(ArtifactId value) { id_ = std::move(value); }
  void set_component(ComponentId value) { component_ = std::move(value); }
  void set_version(Version value) { version_ = std::move(value); }
  void set_build(BuildId value) { build_ = std::move(value); }
  void set_component_type(ComponentType value) { component_type_ = value; }
  void set_digest(Digest value) { digest_ = std::move(value); }
  void set_size_bytes(std::uint64_t value) { size_bytes_ = value; }
  void set_provenance(SignerProvenance value) { provenance_ = std::move(value); }
  void set_platforms(std::vector<std::string> value) { platforms_ = std::move(value); }
  void set_capabilities(std::vector<CapabilityRequirement> value) {
    capabilities_ = std::move(value);
  }
  void set_versions(SchemaProtocolVersions value) { versions_ = std::move(value); }
  void set_rollback(RollbackCompatibility value) { rollback_ = std::move(value); }
  void set_created_at(Timestamp value) { created_at_ = value; }
  void set_media_type(std::string value) { media_type_ = std::move(value); }

  [[nodiscard]] bool supported_on(std::string_view platform) const;
  [[nodiscard]] Status validate() const;

 private:
  ArtifactId id_;
  ComponentId component_;
  Version version_;
  BuildId build_;
  ComponentType component_type_ = ComponentType::software;
  Digest digest_;
  std::uint64_t size_bytes_ = 0;
  SignerProvenance provenance_;
  std::vector<std::string> platforms_;
  std::vector<CapabilityRequirement> capabilities_;
  SchemaProtocolVersions versions_;
  RollbackCompatibility rollback_;
  Timestamp created_at_;
  std::string media_type_ = "application/octet-stream";
};

// Result of resolving and hashing artifact bytes.
struct [[nodiscard]] IntegrityAssessment {
  bool verified = false;
  Digest observed;
  Digest declared;
  std::uint64_t bytes = 0;
  std::string detail;

  [[nodiscard]] json::Value to_json() const;
};

// Verifies payload bytes against the descriptor digest. This is the only path
// that can produce a verified artifact; a digest mismatch blocks use.
[[nodiscard]] IntegrityAssessment verify_artifact_bytes(const ArtifactDescriptor& descriptor,
                                                        std::string_view payload);

// Verifies that the payload equals the descriptor digest without holding the
// whole artifact in memory.
[[nodiscard]] IntegrityAssessment verify_artifact_file(const ArtifactDescriptor& descriptor,
                                                       const std::string& path);

}  // namespace fum
