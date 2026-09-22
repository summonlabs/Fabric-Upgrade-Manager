// Evidence with explicit freshness and incarnation binding.
#pragma once

#include <string>
#include <utility>

#include "fum/core/ids.hpp"
#include "fum/core/time.hpp"
#include "fum/model/target.hpp"

namespace fum {

enum class EvidenceSource : std::uint8_t {
  inventory_probe = 0,
  adapter_observation,
  health_probe,
  compatibility_registry,
  drain_fabric,
  configuration_fabric,
  operator_declaration,
  persisted_record,
};

[[nodiscard]] const char* evidence_source_name(EvidenceSource source) noexcept;

// Evidence is never fresh merely because it can be deserialized: freshness is a
// function of the observing incarnation, the observation time and the declared
// validity window.
template <class T>
class [[nodiscard]] Evidence {
 public:
  Evidence() = default;

  Evidence(T value, Timestamp observed_at, Duration validity, Incarnation incarnation,
           EvidenceSource source, std::string origin)
      : value_(std::move(value)),
        observed_at_(observed_at),
        validity_(validity),
        incarnation_(incarnation),
        source_(source),
        origin_(std::move(origin)) {}

  [[nodiscard]] const T& value() const noexcept { return value_; }
  [[nodiscard]] T& value() noexcept { return value_; }
  [[nodiscard]] Timestamp observed_at() const noexcept { return observed_at_; }
  [[nodiscard]] Duration validity() const noexcept { return validity_; }
  [[nodiscard]] Incarnation incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] EvidenceSource source() const noexcept { return source_; }
  [[nodiscard]] const std::string& origin() const noexcept { return origin_; }
  [[nodiscard]] bool present() const noexcept { return observed_at_.unix_nanos() != 0; }

  [[nodiscard]] FreshnessVerdict freshness(Timestamp now, Incarnation current) const {
    if (!present()) {
      FreshnessVerdict verdict;
      verdict.fresh = false;
      verdict.reason = "no evidence has been observed";
      return verdict;
    }
    return evaluate_freshness(observed_at_, validity_, incarnation_, now, current);
  }

  [[nodiscard]] bool is_fresh(Timestamp now, Incarnation current) const {
    return freshness(now, current).fresh;
  }

 private:
  T value_{};
  Timestamp observed_at_;
  Duration validity_;
  Incarnation incarnation_;
  EvidenceSource source_ = EvidenceSource::inventory_probe;
  std::string origin_;
};

}  // namespace fum
