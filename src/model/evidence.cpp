#include "fum/model/evidence.hpp"

namespace fum {

const char* evidence_source_name(EvidenceSource source) noexcept {
  switch (source) {
    case EvidenceSource::inventory_probe: return "inventory-probe";
    case EvidenceSource::adapter_observation: return "adapter-observation";
    case EvidenceSource::health_probe: return "health-probe";
    case EvidenceSource::compatibility_registry: return "compatibility-registry";
    case EvidenceSource::drain_fabric: return "drain-fabric";
    case EvidenceSource::configuration_fabric: return "configuration-fabric";
    case EvidenceSource::operator_declaration: return "operator-declaration";
    case EvidenceSource::persisted_record: return "persisted-record";
  }
  return "unknown";
}

}  // namespace fum
