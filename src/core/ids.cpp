#include "fum/core/ids.hpp"

#include "fum/core/hash.hpp"

namespace fum {
namespace {
constexpr std::size_t kMaxIdentifierLength = 128;
constexpr std::size_t kMaxNamespaceLength = 64;
}  // namespace

Status validate_identifier(std::string_view text, std::string_view what) {
  if (text.empty()) {
    return make_error(ErrorCode::invalid_argument, std::string(what) + " must not be empty");
  }
  if (text.size() > kMaxIdentifierLength) {
    return make_error(ErrorCode::invalid_argument,
                      std::string(what) + " is longer than the permitted bound",
                      std::to_string(text.size()) + " > " + std::to_string(kMaxIdentifierLength));
  }
  for (const char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-' || c == '_' || c == ':' || c == '/' || c == '@';
    if (!ok) {
      return make_error(ErrorCode::invalid_argument,
                        std::string(what) + " contains a character outside the permitted set",
                        std::string(1, c));
    }
  }
  return ok_status();
}

Result<Digest> Digest::parse(std::string_view hex) {
  // The empty digest is the genesis marker: it means "no predecessor" in the
  // journal hash chain and in the provenance ledger.
  if (hex.empty()) {
    return Digest{};
  }
  FUM_TRYV(hash::from_hex(hex));
  return Digest(std::string(hex));
}

Digest Digest::of_bytes(std::string_view payload) { return Digest(hash::sha256_hex(payload)); }

Digest Digest::of_text(std::string_view payload) { return Digest(hash::sha256_hex(payload)); }

Digest Digest::combine(const Digest& a, const Digest& b) {
  std::string material;
  material.reserve(a.hex().size() + b.hex().size() + 1);
  material.append(a.hex());
  material.push_back(':');
  material.append(b.hex());
  return Digest(hash::sha256_hex(material));
}

std::string to_string(const FenceToken& token) {
  std::string out;
  out.reserve(128);
  out.append("campaign=");
  out.append(token.campaign.str().empty() ? "<none>" : token.campaign.str());
  out.append(" generation=");
  out.append(std::to_string(token.generation.value()));
  out.append(" incarnation=");
  out.append(std::to_string(token.incarnation.value()));
  out.append(" attempt=");
  out.append(token.attempt.str().empty() ? "<none>" : token.attempt.str());
  out.append(" authority=");
  out.append(token.authority.str().empty() ? "<none>" : token.authority.str());
  return out;
}

}  // namespace fum
