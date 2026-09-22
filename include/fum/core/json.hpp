// Deterministic JSON value, strict bounded parser, canonical writer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fum/core/result.hpp"

namespace fum::json {

struct Limits {
  std::size_t max_depth = 64;
  std::size_t max_string_bytes = 1u << 20;
  std::size_t max_nodes = 1u << 20;
  std::size_t max_input_bytes = 1u << 26;
};

class [[nodiscard]] Value {
 public:
  enum class Kind : std::uint8_t { null_value, boolean, integer, unsigned_integer, real, string, array, object };

  using Array = std::vector<Value>;
  using Member = std::pair<std::string, Value>;
  using Object = std::vector<Member>;

  Value() = default;

  [[nodiscard]] static Value make_null() { return Value(); }
  [[nodiscard]] static Value make_bool(bool v);
  [[nodiscard]] static Value make_int(std::int64_t v);
  [[nodiscard]] static Value make_uint(std::uint64_t v);
  [[nodiscard]] static Value make_real(double v);
  [[nodiscard]] static Value make_string(std::string v);
  [[nodiscard]] static Value make_array();
  [[nodiscard]] static Value make_object();

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::null_value; }
  [[nodiscard]] bool is_bool() const noexcept { return kind_ == Kind::boolean; }
  [[nodiscard]] bool is_number() const noexcept {
    return kind_ == Kind::integer || kind_ == Kind::unsigned_integer || kind_ == Kind::real;
  }
  [[nodiscard]] bool is_string() const noexcept { return kind_ == Kind::string; }
  [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::array; }
  [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::object; }

  [[nodiscard]] Result<bool> as_bool() const;
  [[nodiscard]] Result<std::int64_t> as_int() const;
  [[nodiscard]] Result<std::uint64_t> as_uint() const;
  [[nodiscard]] Result<double> as_double() const;
  [[nodiscard]] Result<std::string_view> as_string() const;
  [[nodiscard]] const Array& items() const;
  [[nodiscard]] const Object& members() const;

  // Object access. Missing members return nullptr / not_found.
  [[nodiscard]] const Value* find(std::string_view key) const;
  [[nodiscard]] Result<std::string_view> require_string(std::string_view key) const;
  [[nodiscard]] Result<std::uint64_t> require_uint(std::string_view key) const;
  [[nodiscard]] Result<bool> require_bool(std::string_view key) const;
  [[nodiscard]] Result<double> require_double(std::string_view key) const;
  void set(std::string key, Value value);          // replace in place, else append
  void erase(std::string_view key);
  void push(Value value);

  [[nodiscard]] std::string dump() const;                  // canonical, compact
  [[nodiscard]] std::string dump_pretty(int indent = 2) const;

  friend bool operator==(const Value& a, const Value& b);
  friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }

 private:
  void dump_into(std::string& out, int indent, int depth) const;

  Kind kind_ = Kind::null_value;
  bool bool_ = false;
  std::int64_t int_ = 0;
  std::uint64_t uint_ = 0;
  double real_ = 0.0;
  std::string string_;
  Array array_;
  Object object_;
};

[[nodiscard]] Result<Value> parse(std::string_view text, const Limits& limits = {});
[[nodiscard]] Result<Value> parse_object(std::string_view text, const Limits& limits = {});

}  // namespace fum::json
