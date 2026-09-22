// Strongly typed domain identities. Nothing important is an interchangeable
// string or integer.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "fum/core/result.hpp"

namespace fum {

// Charset and length rules are identical for every identity so that persisted
// identifiers stay comparable and log-safe.
[[nodiscard]] Status validate_identifier(std::string_view text, std::string_view what);

template <class Tag>
class [[nodiscard]] StrongId {
 public:
  StrongId() = default;

  [[nodiscard]] static Result<StrongId> parse(std::string_view text) {
    FUM_TRYV(validate_identifier(text, "identifier"));
    return StrongId(std::string(text));
  }

  // Constructs from an already validated value (internal call sites only).
  [[nodiscard]] static StrongId trusted(std::string text) { return StrongId(std::move(text)); }

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] const std::string& str() const noexcept { return value_; }

  friend bool operator==(const StrongId& a, const StrongId& b) { return a.value_ == b.value_; }
  friend bool operator!=(const StrongId& a, const StrongId& b) { return !(a == b); }
  friend bool operator<(const StrongId& a, const StrongId& b) { return a.value_ < b.value_; }

  [[nodiscard]] std::size_t hash() const noexcept { return std::hash<std::string>{}(value_); }

 private:
  explicit StrongId(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

#define FUM_DECLARE_ID(name, tag)                       \
  struct tag {};                                        \
  using name = StrongId<tag>;                           \
  [[nodiscard]] inline name make_##name(std::string v); \
  [[nodiscard]] inline name make_##name(std::string v) { return name::trusted(std::move(v)); }

FUM_DECLARE_ID(ComponentId, ComponentTag)
FUM_DECLARE_ID(ArtifactId, ArtifactTag)
FUM_DECLARE_ID(BuildId, BuildTag)
FUM_DECLARE_ID(TargetId, TargetTag)
FUM_DECLARE_ID(CampaignId, CampaignTag)
FUM_DECLARE_ID(StageId, StageTag)
FUM_DECLARE_ID(AttemptId, AttemptTag)
FUM_DECLARE_ID(AuthorityId, AuthorityTag)
FUM_DECLARE_ID(PolicyId, PolicyTag)
FUM_DECLARE_ID(EvidenceId, EvidenceTag)
FUM_DECLARE_ID(DecisionId, DecisionTag)
FUM_DECLARE_ID(StepId, StepTag)
FUM_DECLARE_ID(RecordId, RecordTag)
FUM_DECLARE_ID(AdapterId, AdapterTag)
FUM_DECLARE_ID(CorrelationId, CorrelationTag)
FUM_DECLARE_ID(SignerId, SignerTag)

#undef FUM_DECLARE_ID

// A validated SHA-256 digest rendered as lowercase hex.
class [[nodiscard]] Digest {
 public:
  Digest() = default;
  [[nodiscard]] static Result<Digest> parse(std::string_view hex);
  [[nodiscard]] static Digest of_bytes(std::string_view payload);
  [[nodiscard]] static Digest of_text(std::string_view payload);
  [[nodiscard]] static Digest combine(const Digest& a, const Digest& b);

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] const std::string& hex() const noexcept { return value_; }
  [[nodiscard]] const char* algorithm() const noexcept { return "sha256"; }

  friend bool operator==(const Digest& a, const Digest& b) { return a.value_ == b.value_; }
  friend bool operator!=(const Digest& a, const Digest& b) { return !(a == b); }
  friend bool operator<(const Digest& a, const Digest& b) { return a.value_ < b.value_; }
  [[nodiscard]] std::size_t hash() const noexcept { return std::hash<std::string>{}(value_); }

 private:
  explicit Digest(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

// Monotonic counters used for generations, epochs and process incarnations.
// They are not interchangeable: a generation is never an epoch.
template <class Tag>
class [[nodiscard]] Counter {
 public:
  using value_type = std::uint64_t;

  constexpr Counter() = default;
  constexpr explicit Counter(value_type value) : value_(value) {}

  [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_initial() const noexcept { return value_ <= 1; }

  [[nodiscard]] Result<Counter> try_next() const {
    if (value_ == UINT64_MAX) {
      return make_error(ErrorCode::resource_exhausted, "counter exhausted");
    }
    return Counter(value_ + 1);
  }

  friend constexpr bool operator==(const Counter& a, const Counter& b) {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Counter& a, const Counter& b) { return !(a == b); }
  friend constexpr bool operator<(const Counter& a, const Counter& b) {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator<=(const Counter& a, const Counter& b) {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>(const Counter& a, const Counter& b) { return b < a; }
  friend constexpr bool operator>=(const Counter& a, const Counter& b) { return b <= a; }

 private:
  value_type value_ = 0;
};

struct GenerationTag {};
struct EpochTag {};
struct IncarnationTag {};
struct RevisionTag {};
struct SequenceTag {};

using Generation = Counter<GenerationTag>;
using Epoch = Counter<EpochTag>;
using Incarnation = Counter<IncarnationTag>;
using Revision = Counter<RevisionTag>;
using Sequence = Counter<SequenceTag>;

// Fencing token: everything that can mutate campaign state carries one.
struct [[nodiscard]] FenceToken {
  CampaignId campaign;
  Generation generation;
  Incarnation incarnation;
  AttemptId attempt;
  AuthorityId authority;

  friend bool operator==(const FenceToken& a, const FenceToken& b) {
    return a.campaign == b.campaign && a.generation == b.generation &&
           a.incarnation == b.incarnation && a.attempt == b.attempt &&
           a.authority == b.authority;
  }
};

[[nodiscard]] std::string to_string(const FenceToken& token);

}  // namespace fum

namespace std {
template <class Tag>
struct hash<fum::StrongId<Tag>> {
  size_t operator()(const fum::StrongId<Tag>& id) const noexcept { return id.hash(); }
};
template <>
struct hash<fum::Digest> {
  size_t operator()(const fum::Digest& d) const noexcept { return d.hash(); }
};
}  // namespace std
