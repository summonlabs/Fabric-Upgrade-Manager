#include "runtime_assembly.hpp"

#include <algorithm>

#include "fum/core/fs.hpp"

namespace fum::cli {

FileInventorySource::FileInventorySource(std::string path, AdapterRegistry* registry)
    : path_(std::move(path)), registry_(registry) {}

std::size_t FileInventorySource::adapter_count() const {
  std::size_t count = synthetic_.size();
#if defined(FUM_CLI_HAS_LOCAL_PROCESS)
  count += local_process_.size();
#endif
  return count;
}

Status FileInventorySource::load() {
  if (path_.empty()) {
    return make_error(ErrorCode::not_integrated,
                      "no inventory catalog was provided (--inventory)");
  }
  auto text = fs::read_file(path_, 4ull * 1024 * 1024);
  if (!text.has_value()) {
    return text.error();
  }
  auto document = json::parse_object(text.value());
  if (!document.has_value()) {
    return document.error();
  }
  const json::Value* targets = document.value().find("targets");
  if (targets == nullptr || !targets->is_array()) {
    return make_error(ErrorCode::invalid_argument,
                      "the inventory catalog must declare a targets array", path_);
  }
  targets_.clear();
  for (const auto& entry : targets->items()) {
    TargetDescriptor descriptor;
    FUM_TRY(descriptor, TargetDescriptor::from_json(entry));
    std::string_view platform;
    FUM_TRY(platform, entry.require_string("platform"));
    descriptor.platform = std::string(platform);
    targets_.push_back(descriptor);

    if (descriptor.adapter_kind == AdapterKind::synthetic) {
      auto existing = synthetic_.find(descriptor.adapter.str());
      if (existing == synthetic_.end()) {
        auto adapter = std::make_shared<SyntheticAdapter>(descriptor.adapter);
        adapter->set_claims(descriptor.claims);
        synthetic_[descriptor.adapter.str()] = adapter;
        registry_->add(adapter);
        existing = synthetic_.find(descriptor.adapter.str());
      }
      SyntheticTarget target;
      target.id = descriptor.id;
      target.component = descriptor.component;
      target.display_name = descriptor.display_name;
      target.version = descriptor.installed_version;
      target.build = descriptor.installed_build;
      target.platform = descriptor.platform;
      target.capabilities = descriptor.capabilities;
      target.authority = descriptor.authority;
      target.redundancy_group = descriptor.redundancy_group;
      target.generation_capable = descriptor.generation_capable;
      existing->second->add_target(target);
      continue;
    }

#if defined(FUM_CLI_HAS_LOCAL_PROCESS)
    if (descriptor.adapter_kind == AdapterKind::local_process) {
      if (control_plane_executable_.empty()) {
        return make_error(ErrorCode::invalid_argument,
                          "the catalog declares local-process targets but no control-plane "
                          "executable was provided (--control-plane)",
                          descriptor.id.str());
      }
      std::string_view root;
      FUM_TRY(root, entry.require_string("root_directory"));
      auto existing = local_process_.find(descriptor.adapter.str());
      if (existing == local_process_.end()) {
        LocalProcessOptions options;
        options.control_plane_executable = control_plane_executable_;
        auto adapter =
            std::make_shared<LocalProcessAdapter>(descriptor.adapter, options);
        local_process_[descriptor.adapter.str()] = adapter;
        registry_->add(adapter);
        existing = local_process_.find(descriptor.adapter.str());
      }
      LocalProcessTarget target;
      target.id = descriptor.id;
      target.component = descriptor.component;
      target.display_name = descriptor.display_name;
      target.version = descriptor.installed_version;
      target.build = descriptor.installed_build;
      target.platform = descriptor.platform;
      target.capabilities = descriptor.capabilities;
      target.authority = descriptor.authority;
      target.redundancy_group = descriptor.redundancy_group;
      target.generation_capable = descriptor.generation_capable;
      target.root_directory = fs::is_absolute(root) ? std::string(root)
                                                    : fs::join(fs::parent_directory(path_),
                                                               std::string(root));
      FUM_TRYV(fs::ensure_directory(target.root_directory));
      existing->second->add_target(target);
      continue;
    }
#endif
    return make_error(ErrorCode::unsupported,
                      "the catalog declares an adapter kind this build cannot bind",
                      adapter_kind_name(descriptor.adapter_kind));
  }
  return ok_status();
}

Result<std::vector<TargetDescriptor>> FileInventorySource::resolve(Timestamp now,
                                                                  Incarnation incarnation) {
  static_cast<void>(now);
  static_cast<void>(incarnation);
  return targets_;
}

FileCompatibilityRegistry::FileCompatibilityRegistry(std::string path) : path_(std::move(path)) {}

Status FileCompatibilityRegistry::load() {
  if (path_.empty()) {
    return make_error(ErrorCode::not_integrated,
                      "no compatibility registry export was provided (--registry)");
  }
  auto text = fs::read_file(path_, 4ull * 1024 * 1024);
  if (!text.has_value()) {
    return text.error();
  }
  auto document = json::parse_object(text.value());
  if (!document.has_value()) {
    return document.error();
  }
  if (const json::Value* revision = document.value().find("revision"); revision != nullptr) {
    revision_ = std::string(revision->as_string().value_or("file-registry"));
  }
  if (const json::Value* fallback = document.value().find("default_compatible");
      fallback != nullptr) {
    default_compatible_ = fallback->as_bool().value_or(false);
  }
  const json::Value* entries = document.value().find("entries");
  if (entries == nullptr || !entries->is_array()) {
    return make_error(ErrorCode::invalid_argument,
                      "the compatibility export must declare an entries array", path_);
  }
  entries_.clear();
  for (const auto& item : entries->items()) {
    CompatibilityMatrixEntry entry;
    std::string text_value;
    FUM_TRY(text_value, item.require_string("component_id"));
    FUM_TRY(entry.component, ComponentId::parse(text_value));
    std::string_view version_text;
    FUM_TRY(version_text, item.require_string("from_version"));
    FUM_TRY(entry.from, Version::parse(version_text));
    FUM_TRY(version_text, item.require_string("to_version"));
    FUM_TRY(entry.to, Version::parse(version_text));
    FUM_TRY(entry.compatible, item.require_bool("compatible"));
    if (const json::Value* reason = item.find("reason"); reason != nullptr) {
      entry.reason = std::string(reason->as_string().value_or(""));
    }
    if (const json::Value* rollback = item.find("rollback_supported"); rollback != nullptr) {
      entry.rollback_supported = rollback->as_bool().value_or(false);
    }
    if (const json::Value* boundary = item.find("rollback_boundary"); boundary != nullptr) {
      entry.rollback_boundary = std::string(boundary->as_string().value_or(""));
    }
    entries_.push_back(std::move(entry));
  }
  return ok_status();
}

Result<CompatibilityVerdict> FileCompatibilityRegistry::query(const CompatibilityQuery& request) {
  CompatibilityVerdict verdict;
  verdict.answered = true;
  verdict.registry_revision = revision_;
  verdict.registry_policy = "registry-export";
  verdict.validity = Duration::from_seconds(600);
  verdict.compatible = default_compatible_;
  for (const auto& entry : entries_) {
    if (entry.component == request.component && entry.from == request.from_version &&
        entry.to == request.to_version) {
      verdict.compatible = entry.compatible;
      verdict.rollback_supported = entry.rollback_supported;
      verdict.rollback_boundary = entry.rollback_boundary;
      verdict.evaluated_constraints.push_back("component=" + request.component.str());
      verdict.evaluated_constraints.push_back("from=" + request.from_version.text());
      verdict.evaluated_constraints.push_back("to=" + request.to_version.text());
      if (!entry.compatible) {
        verdict.blockers.push_back(entry.reason.empty() ? "declared incompatible by the registry"
                                                        : entry.reason);
      }
      verdict.detail = "resolved from the registry export";
      return verdict;
    }
  }
  verdict.blockers.push_back("the registry export has no entry for " +
                             request.from_version.text() + " -> " + request.to_version.text());
  verdict.detail = "no entry";
  return verdict;
}

FileArtifactSource::FileArtifactSource(std::string catalog_path) : path_(std::move(catalog_path)) {}

Status FileArtifactSource::load() {
  if (path_.empty()) {
    return make_error(ErrorCode::not_integrated,
                      "no artifact catalog was provided (--artifacts)");
  }
  auto text = fs::read_file(path_, 4ull * 1024 * 1024);
  if (!text.has_value()) {
    return text.error();
  }
  auto document = json::parse_object(text.value());
  if (!document.has_value()) {
    return document.error();
  }
  const json::Value* artifacts = document.value().find("artifacts");
  if (artifacts == nullptr || !artifacts->is_array()) {
    return make_error(ErrorCode::invalid_argument,
                      "the artifact catalog must declare an artifacts array", path_);
  }
  payloads_.clear();
  const std::string base = fs::parent_directory(path_);
  for (const auto& item : artifacts->items()) {
    std::string_view id;
    FUM_TRY(id, item.require_string("artifact_id"));
    std::string_view payload;
    FUM_TRY(payload, item.require_string("payload_path"));
    std::string resolved(payload);
    if (!fs::is_absolute(resolved)) {
      resolved = fs::join(base, resolved);
    }
    payloads_[std::string(id)] = resolved;
  }
  return ok_status();
}

Result<ArtifactPayload> FileArtifactSource::resolve(const ArtifactDescriptor& artifact) {
  ArtifactPayload payload;
  const auto it = payloads_.find(artifact.id().str());
  if (it == payloads_.end()) {
    payload.present = false;
    payload.detail = "the artifact catalog has no payload for " + artifact.id().str();
    return payload;
  }
  if (!fs::exists(it->second)) {
    payload.present = false;
    payload.detail = "the artifact payload file is missing: " + it->second;
    return payload;
  }
  payload.path = it->second;
  auto size = fs::file_size(it->second);
  payload.bytes = size.has_value() ? size.value() : 0;
  payload.present = true;
  payload.detail = "resolved from the artifact catalog";
  return payload;
}

Result<Assembly> assemble(const AssemblyOptions& options) {
  Assembly assembly;
  assembly.config.store.directory = options.state_directory;
  assembly.config.executor = options.executor;
  assembly.config.worker_threads = options.worker_threads;
  assembly.config.policy.id = make_PolicyId("fum-cli-policy");
  assembly.config.policy.revision = Revision(1);
  assembly.sink = std::make_shared<MemoryLogSink>();
  assembly.logger =
      std::make_shared<Logger>(assembly.sink, options.verbose ? LogLevel::debug : LogLevel::info);
  assembly.config.logger = assembly.logger.get();

  assembly.inventory = std::make_shared<FileInventorySource>(options.inventory_path,
                                                             &assembly.dependencies.adapters);
  assembly.inventory->set_control_plane_executable(options.control_plane_executable);
  assembly.registry = std::make_shared<FileCompatibilityRegistry>(options.registry_path);
  assembly.artifacts = std::make_shared<FileArtifactSource>(options.artifacts_path);

  FUM_TRYV(assembly.artifacts->load());
  FUM_TRYV(assembly.registry->load());
  FUM_TRYV(assembly.inventory->load());
  if (assembly.inventory->adapter_count() == 0) {
    return make_error(ErrorCode::invalid_argument,
                      "the inventory catalog declares no target that this build can bind");
  }

  assembly.dependencies.inventory = assembly.inventory;
  assembly.dependencies.compatibility = assembly.registry;
  assembly.dependencies.artifacts = assembly.artifacts;
  assembly.dependencies.clock = std::make_shared<SystemClock>();
  if (options.drain_integrated) {
    assembly.dependencies.drain = std::make_shared<SyntheticDrainFabric>();
  }
  if (options.configuration_integrated) {
    assembly.dependencies.configuration = std::make_shared<SyntheticConfigurationFabric>();
  }
  if (options.rollout_integrated) {
    assembly.dependencies.rollout = std::make_shared<SyntheticRolloutFabric>();
  }
  return assembly;
}

}  // namespace fum::cli
