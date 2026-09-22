// Component version identity and constraint algebra.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fum/core/result.hpp"

namespace fum {

// Dotted numeric version with an optional prerelease tag: 1.2.3, 1.2.3-rc1.
class [[nodiscard]] Version {
 public:
  Version() = default;
  Version(std::uint32_t major, std::uint32_t minor, std::uint32_t patch);
  Version(std::uint32_t major, std::uint32_t minor, std::uint32_t patch,
          std::string prerelease);

  [[nodiscard]] static Result<Version> parse(std::string_view text);

  [[nodiscard]] std::uint32_t major() const noexcept { return major_; }
  [[nodiscard]] std::uint32_t minor() const noexcept { return minor_; }
  [[nodiscard]] std::uint32_t patch() const noexcept { return patch_; }
  [[nodiscard]] const std::string& prerelease() const noexcept { return prerelease_; }
  [[nodiscard]] bool is_prerelease() const noexcept { return !prerelease_.empty(); }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] const std::string& text() const noexcept { return text_; }

  // Number of releases apart when both versions share a major line. Used for
  // version skew accounting; negative when `other` is newer.
  [[nodiscard]] std::int64_t release_distance(const Version& other) const noexcept;
  [[nodiscard]] bool same_major(const Version& other) const noexcept {
    return major_ == other.major_;
  }

  friend bool operator==(const Version& a, const Version& b) { return a.compare(b) == 0; }
  friend bool operator!=(const Version& a, const Version& b) { return a.compare(b) != 0; }
  friend bool operator<(const Version& a, const Version& b) { return a.compare(b) < 0; }
  friend bool operator<=(const Version& a, const Version& b) { return a.compare(b) <= 0; }
  friend bool operator>(const Version& a, const Version& b) { return a.compare(b) > 0; }
  friend bool operator>=(const Version& a, const Version& b) { return a.compare(b) >= 0; }
  [[nodiscard]] int compare(const Version& other) const noexcept;

 private:
  void refresh_text();
  std::uint32_t major_ = 0;
  std::uint32_t minor_ = 0;
  std::uint32_t patch_ = 0;
  std::string prerelease_;
  std::string text_;
};

enum class ConstraintOperator : std::uint8_t {
  equal,
  not_equal,
  less,
  less_equal,
  greater,
  greater_equal,
  compatible_major,   // ^1.2.3
  compatible_minor,   // ~1.2.3
};

[[nodiscard]] const char* constraint_operator_symbol(ConstraintOperator op) noexcept;

struct [[nodiscard]] VersionConstraint {
  ConstraintOperator op = ConstraintOperator::equal;
  Version version;

  [[nodiscard]] static Result<VersionConstraint> parse(std::string_view text);
  [[nodiscard]] bool matches(const Version& candidate) const;
  [[nodiscard]] std::string to_string() const;  // canonical deterministic rendering

  friend bool operator==(const VersionConstraint& a, const VersionConstraint& b) {
    return a.op == b.op && a.version == b.version;
  }
};

// Conjunction of constraints: every constraint must match.
class [[nodiscard]] VersionRange {
 public:
  VersionRange() = default;
  explicit VersionRange(std::vector<VersionConstraint> constraints);

  [[nodiscard]] static Result<VersionRange> parse(std::string_view text);
  [[nodiscard]] bool empty() const noexcept { return constraints_.empty(); }
  [[nodiscard]] const std::vector<VersionConstraint>& constraints() const noexcept {
    return constraints_;
  }
  [[nodiscard]] bool admits(const Version& candidate) const;
  [[nodiscard]] std::string to_string() const;

 private:
  std::vector<VersionConstraint> constraints_;
};

}  // namespace fum
