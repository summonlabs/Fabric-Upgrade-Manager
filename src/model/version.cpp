#include "fum/model/version.hpp"

#include <charconv>
#include <cstdio>
#include <limits>

namespace fum {
namespace {

Result<std::uint32_t> parse_component(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::invalid_argument, "version component is empty");
  }
  if (text.size() > 1 && text.front() == '0') {
    return make_error(ErrorCode::invalid_argument, "version component has a leading zero",
                      std::string(text));
  }
  std::uint32_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return make_error(ErrorCode::invalid_argument, "version component is not a number",
                      std::string(text));
  }
  return value;
}

bool valid_prerelease(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

}  // namespace

Version::Version(std::uint32_t major, std::uint32_t minor, std::uint32_t patch)
    : major_(major), minor_(minor), patch_(patch) {
  refresh_text();
}

Version::Version(std::uint32_t major, std::uint32_t minor, std::uint32_t patch,
                 std::string prerelease)
    : major_(major), minor_(minor), patch_(patch), prerelease_(std::move(prerelease)) {
  refresh_text();
}

void Version::refresh_text() {
  text_ = std::to_string(major_) + "." + std::to_string(minor_) + "." + std::to_string(patch_);
  if (!prerelease_.empty()) {
    text_.push_back('-');
    text_.append(prerelease_);
  }
}

Result<Version> Version::parse(std::string_view text) {
  if (text.empty() || text.size() > 64) {
    return make_error(ErrorCode::invalid_argument, "version text length is out of range",
                      std::string(text));
  }
  const std::size_t first_dot = text.find('.');
  if (first_dot == std::string_view::npos) {
    return make_error(ErrorCode::invalid_argument, "version must have major.minor.patch",
                      std::string(text));
  }
  const std::size_t second_dot = text.find('.', first_dot + 1);
  if (second_dot == std::string_view::npos) {
    return make_error(ErrorCode::invalid_argument, "version must have major.minor.patch",
                      std::string(text));
  }
  std::string_view patch_part = text.substr(second_dot + 1);
  std::string prerelease;
  const std::size_t dash = patch_part.find('-');
  if (dash != std::string_view::npos) {
    prerelease = std::string(patch_part.substr(dash + 1));
    patch_part = patch_part.substr(0, dash);
    if (!valid_prerelease(prerelease)) {
      return make_error(ErrorCode::invalid_argument, "version prerelease tag is malformed",
                        std::string(text));
    }
  }
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
  FUM_TRY(major, parse_component(text.substr(0, first_dot)));
  FUM_TRY(minor, parse_component(text.substr(first_dot + 1, second_dot - first_dot - 1)));
  FUM_TRY(patch, parse_component(patch_part));
  return Version(major, minor, patch, std::move(prerelease));
}

int Version::compare(const Version& other) const noexcept {
  if (major_ != other.major_) return major_ < other.major_ ? -1 : 1;
  if (minor_ != other.minor_) return minor_ < other.minor_ ? -1 : 1;
  if (patch_ != other.patch_) return patch_ < other.patch_ ? -1 : 1;
  if (prerelease_ == other.prerelease_) return 0;
  if (prerelease_.empty()) return 1;    // release outranks prerelease
  if (other.prerelease_.empty()) return -1;
  return prerelease_ < other.prerelease_ ? -1 : 1;
}

std::int64_t Version::release_distance(const Version& other) const noexcept {
  // Total ordering proxy used for skew accounting: major/minor/patch deltas.
  const std::int64_t self =
      static_cast<std::int64_t>(major_) * 1000000 + static_cast<std::int64_t>(minor_) * 1000 +
      static_cast<std::int64_t>(patch_);
  const std::int64_t that = static_cast<std::int64_t>(other.major_) * 1000000 +
                            static_cast<std::int64_t>(other.minor_) * 1000 +
                            static_cast<std::int64_t>(other.patch_);
  return self - that;
}

const char* constraint_operator_symbol(ConstraintOperator op) noexcept {
  switch (op) {
    case ConstraintOperator::equal: return "=";
    case ConstraintOperator::not_equal: return "!=";
    case ConstraintOperator::less: return "<";
    case ConstraintOperator::less_equal: return "<=";
    case ConstraintOperator::greater: return ">";
    case ConstraintOperator::greater_equal: return ">=";
    case ConstraintOperator::compatible_major: return "^";
    case ConstraintOperator::compatible_minor: return "~";
  }
  return "?";
}

Result<VersionConstraint> VersionConstraint::parse(std::string_view text) {
  std::string_view body = text;
  while (!body.empty() && body.front() == ' ') {
    body.remove_prefix(1);
  }
  while (!body.empty() && body.back() == ' ') {
    body.remove_suffix(1);
  }
  ConstraintOperator op = ConstraintOperator::equal;
  std::size_t consumed = 0;
  if (body.size() >= 2 && body.substr(0, 2) == "!=") {
    op = ConstraintOperator::not_equal;
    consumed = 2;
  } else if (body.size() >= 2 && body.substr(0, 2) == ">=") {
    op = ConstraintOperator::greater_equal;
    consumed = 2;
  } else if (body.size() >= 2 && body.substr(0, 2) == "<=") {
    op = ConstraintOperator::less_equal;
    consumed = 2;
  } else if (!body.empty() && body.front() == '>') {
    op = ConstraintOperator::greater;
    consumed = 1;
  } else if (!body.empty() && body.front() == '<') {
    op = ConstraintOperator::less;
    consumed = 1;
  } else if (!body.empty() && body.front() == '=') {
    op = ConstraintOperator::equal;
    consumed = 1;
  } else if (!body.empty() && body.front() == '^') {
    op = ConstraintOperator::compatible_major;
    consumed = 1;
  } else if (!body.empty() && body.front() == '~') {
    op = ConstraintOperator::compatible_minor;
    consumed = 1;
  }
  std::string_view version_text = body.substr(consumed);
  while (!version_text.empty() && version_text.front() == ' ') {
    version_text.remove_prefix(1);
  }
  Version parsed;
  FUM_TRY(parsed, Version::parse(version_text));
  VersionConstraint constraint;
  constraint.op = op;
  constraint.version = std::move(parsed);
  return constraint;
}

bool VersionConstraint::matches(const Version& candidate) const {
  const int order = candidate.compare(version);
  switch (op) {
    case ConstraintOperator::equal: return order == 0;
    case ConstraintOperator::not_equal: return order != 0;
    case ConstraintOperator::less: return order < 0;
    case ConstraintOperator::less_equal: return order <= 0;
    case ConstraintOperator::greater: return order > 0;
    case ConstraintOperator::greater_equal: return order >= 0;
    case ConstraintOperator::compatible_major:
      return candidate.major() == version.major() && order >= 0;
    case ConstraintOperator::compatible_minor:
      return candidate.major() == version.major() && candidate.minor() == version.minor() &&
             order >= 0;
  }
  return false;
}

std::string VersionConstraint::to_string() const {
  return std::string(constraint_operator_symbol(op)) + version.text();
}

VersionRange::VersionRange(std::vector<VersionConstraint> constraints)
    : constraints_(std::move(constraints)) {}

Result<VersionRange> VersionRange::parse(std::string_view text) {
  std::vector<VersionConstraint> constraints;
  std::size_t start = 0;
  if (text.size() > 512) {
    return make_error(ErrorCode::invalid_argument, "version range text is too long");
  }
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string_view piece =
        comma == std::string_view::npos ? text.substr(start) : text.substr(start, comma - start);
    if (!piece.empty()) {
      VersionConstraint constraint;
      FUM_TRY(constraint, VersionConstraint::parse(piece));
      constraints.push_back(std::move(constraint));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  if (constraints.empty()) {
    return make_error(ErrorCode::invalid_argument, "version range is empty");
  }
  return VersionRange(std::move(constraints));
}

bool VersionRange::admits(const Version& candidate) const {
  if (constraints_.empty()) {
    return true;  // no declared constraint: anything the registry allows
  }
  for (const auto& constraint : constraints_) {
    if (!constraint.matches(candidate)) {
      return false;
    }
  }
  return true;
}

std::string VersionRange::to_string() const {
  std::string out;
  for (std::size_t i = 0; i < constraints_.size(); ++i) {
    if (i != 0) {
      out.append(",");
    }
    out.append(constraints_[i].to_string());
  }
  return out;
}

}  // namespace fum
