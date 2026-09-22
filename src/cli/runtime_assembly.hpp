// CLI runtime assembly: file-backed integration ports and adapters.
//
// Fabric Upgrade Manager does not own compatibility knowledge, rollout
// sequencing, service removal or configuration delivery. The CLI binds those
// ports to operator-provided integration sources (catalog and export files) and
// reports not_integrated for anything it cannot bind, instead of inventing an
// answer.
//
// Adapters are bound from the catalog: each target declares its adapter
// identity, kind and claims, and the CLI instantiates exactly what the catalog
// declares.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fum/adapters/synthetic_adapter.hpp"
#include "fum/engine/engine.hpp"

#if defined(FUM_CLI_HAS_LOCAL_PROCESS)
#include "fum/adapters/local_process/local_process_adapter.hpp"
#endif

namespace fum::cli {

// Inventory resolved from an operator-provided catalog file. The catalog also
// declares which adapters exist and what they claim.
class FileInventorySource : public InventorySourcePort {
 public:
  FileInventorySource(std::string path, AdapterRegistry* registry);
  [[nodiscard]] std::string id() const override { return "file-inventory"; }
  [[nodiscard]] Result<std::vector<TargetDescriptor>> resolve(Timestamp now,
                                                             Incarnation incarnation) override;
  [[nodiscard]] Status load();
  void set_control_plane_executable(std::string path) {
    control_plane_executable_ = std::move(path);
  }
  [[nodiscard]] std::size_t adapter_count() const;

 private:
  std::string path_;
  AdapterRegistry* registry_ = nullptr;
  std::string control_plane_executable_;
  std::vector<TargetDescriptor> targets_;
  std::map<std::string, std::shared_ptr<SyntheticAdapter>> synthetic_;
#if defined(FUM_CLI_HAS_LOCAL_PROCESS)
  std::map<std::string, std::shared_ptr<LocalProcessAdapter>> local_process_;
#endif
};

// Compatibility matrix from an operator-provided registry export.
class FileCompatibilityRegistry : public CompatibilityRegistryPort {
 public:
  explicit FileCompatibilityRegistry(std::string path);
  [[nodiscard]] std::string id() const override { return "file-compatibility-registry"; }
  [[nodiscard]] Result<CompatibilityVerdict> query(const CompatibilityQuery& request) override;
  [[nodiscard]] Status load();

 private:
  std::string path_;
  std::string revision_ = "file-registry";
  bool default_compatible_ = false;
  std::vector<CompatibilityMatrixEntry> entries_;
};

// Artifact bytes from an operator-provided catalog of artifact payloads.
class FileArtifactSource : public ArtifactSourcePort {
 public:
  explicit FileArtifactSource(std::string catalog_path);
  [[nodiscard]] std::string id() const override { return "file-artifact-source"; }
  [[nodiscard]] Result<ArtifactPayload> resolve(const ArtifactDescriptor& artifact) override;
  [[nodiscard]] Status load();

 private:
  std::string path_;
  std::map<std::string, std::string> payloads_;
};

struct [[nodiscard]] AssemblyOptions {
  std::string state_directory;
  std::string inventory_path;
  std::string registry_path;
  std::string artifacts_path;
  std::string control_plane_executable;
  bool drain_integrated = true;
  bool configuration_integrated = false;
  bool rollout_integrated = false;
  ExecutorKind executor = ExecutorKind::deterministic_inline;
  std::size_t worker_threads = 4;
  bool verbose = false;
};

struct [[nodiscard]] Assembly {
  EngineConfig config;
  EngineDependencies dependencies;
  std::shared_ptr<FileInventorySource> inventory;
  std::shared_ptr<FileCompatibilityRegistry> registry;
  std::shared_ptr<FileArtifactSource> artifacts;
  std::shared_ptr<fum::Logger> logger;
  std::shared_ptr<MemoryLogSink> sink;
};

// Builds the runtime. Returns an error for options that cannot be honoured.
[[nodiscard]] Result<Assembly> assemble(const AssemblyOptions& options);

}  // namespace fum::cli
