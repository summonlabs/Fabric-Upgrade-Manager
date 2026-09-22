#include "fum/core/result.hpp"

namespace fum {

const char* error_code_name(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::ok: return "ok";
    case ErrorCode::invalid_argument: return "invalid-argument";
    case ErrorCode::not_found: return "not-found";
    case ErrorCode::already_exists: return "already-exists";
    case ErrorCode::conflict: return "conflict";
    case ErrorCode::precondition_failed: return "precondition-failed";
    case ErrorCode::fenced: return "fenced";
    case ErrorCode::stale: return "stale";
    case ErrorCode::integrity_failure: return "integrity-failure";
    case ErrorCode::corrupt: return "corrupt";
    case ErrorCode::io_error: return "io-error";
    case ErrorCode::timeout: return "timeout";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::not_integrated: return "not-integrated";
    case ErrorCode::policy_denied: return "policy-denied";
    case ErrorCode::incompatible: return "incompatible";
    case ErrorCode::irreversible: return "irreversible";
    case ErrorCode::resource_exhausted: return "resource-exhausted";
    case ErrorCode::internal: return "internal";
    case ErrorCode::closed: return "closed";
  }
  return "unknown";
}

Error::Error(ErrorCode code, std::string message, std::string detail)
    : code_(code), message_(std::move(message)), detail_(std::move(detail)) {}

std::string Error::to_string() const {
  std::string out;
  out.append(error_code_name(code_));
  out.append(": ");
  out.append(message_);
  if (!detail_.empty()) {
    out.append(" [");
    out.append(detail_);
    out.append("]");
  }
  return out;
}

}  // namespace fum
