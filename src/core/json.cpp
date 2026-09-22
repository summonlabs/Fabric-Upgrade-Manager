#include "fum/core/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace fum::json {
namespace {

void append_escaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out.append(buffer);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void append_real(std::string& out, double value) {
  if (std::isnan(value) || std::isinf(value)) {
    out.append("null");
    return;
  }
  char buffer[40];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) {
    out.append("null");
    return;
  }
  std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
  out.append(text);
  if (text.find_first_of(".eE") == std::string_view::npos) {
    out.append(".0");
  }
}

class Parser {
 public:
  Parser(std::string_view text, const Limits& limits) : text_(text), limits_(limits) {}

  Result<Value> run() {
    if (text_.size() > limits_.max_input_bytes) {
      return make_error(ErrorCode::resource_exhausted, "json input exceeds the configured bound",
                        std::to_string(text_.size()));
    }
    skip_whitespace();
    Value value;
    FUM_TRYV(parse_value(value, 0));
    skip_whitespace();
    if (pos_ != text_.size()) {
      return make_error(ErrorCode::invalid_argument, "trailing data after json value",
                        "offset " + std::to_string(pos_));
    }
    return value;
  }

 private:
  [[nodiscard]] bool at_end() const { return pos_ >= text_.size(); }
  [[nodiscard]] char peek() const { return text_[pos_]; }

  void skip_whitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  Status count_node() {
    if (++nodes_ > limits_.max_nodes) {
      return make_error(ErrorCode::resource_exhausted, "json document exceeds the node bound");
    }
    return ok_status();
  }

  Status parse_value(Value& out, std::size_t depth) {
    if (depth > limits_.max_depth) {
      return make_error(ErrorCode::resource_exhausted, "json nesting exceeds the depth bound");
    }
    if (at_end()) {
      return make_error(ErrorCode::invalid_argument, "unexpected end of json input");
    }
    FUM_TRYV(count_node());
    switch (peek()) {
      case '{': return parse_object_body(out, depth);
      case '[': return parse_array_body(out, depth);
      case '"': {
        std::string text;
        FUM_TRYV(parse_string(text));
        out = Value::make_string(std::move(text));
        return ok_status();
      }
      case 't':
        if (text_.substr(pos_, 4) == "true") {
          pos_ += 4;
          out = Value::make_bool(true);
          return ok_status();
        }
        break;
      case 'f':
        if (text_.substr(pos_, 5) == "false") {
          pos_ += 5;
          out = Value::make_bool(false);
          return ok_status();
        }
        break;
      case 'n':
        if (text_.substr(pos_, 4) == "null") {
          pos_ += 4;
          out = Value::make_null();
          return ok_status();
        }
        break;
      default:
        return parse_number(out);
    }
    return make_error(ErrorCode::invalid_argument, "invalid json literal",
                      "offset " + std::to_string(pos_));
  }

  Status parse_array_body(Value& out, std::size_t depth) {
    ++pos_;  // '['
    Value array = Value::make_array();
    skip_whitespace();
    if (!at_end() && peek() == ']') {
      ++pos_;
      out = std::move(array);
      return ok_status();
    }
    for (;;) {
      skip_whitespace();
      Value element;
      FUM_TRYV(parse_value(element, depth + 1));
      array.push(std::move(element));
      skip_whitespace();
      if (at_end()) {
        return make_error(ErrorCode::invalid_argument, "unterminated json array");
      }
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        out = std::move(array);
        return ok_status();
      }
      return make_error(ErrorCode::invalid_argument, "expected ',' or ']' in json array",
                        "offset " + std::to_string(pos_));
    }
  }

  Status parse_object_body(Value& out, std::size_t depth) {
    ++pos_;  // '{'
    Value object = Value::make_object();
    skip_whitespace();
    if (!at_end() && peek() == '}') {
      ++pos_;
      out = std::move(object);
      return ok_status();
    }
    for (;;) {
      skip_whitespace();
      if (at_end() || peek() != '"') {
        return make_error(ErrorCode::invalid_argument, "expected a json object key",
                          "offset " + std::to_string(pos_));
      }
      std::string key;
      FUM_TRYV(parse_string(key));
      skip_whitespace();
      if (at_end() || peek() != ':') {
        return make_error(ErrorCode::invalid_argument, "expected ':' after json object key",
                          "offset " + std::to_string(pos_));
      }
      ++pos_;
      skip_whitespace();
      Value value;
      FUM_TRYV(parse_value(value, depth + 1));
      object.set(std::move(key), std::move(value));
      skip_whitespace();
      if (at_end()) {
        return make_error(ErrorCode::invalid_argument, "unterminated json object");
      }
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        out = std::move(object);
        return ok_status();
      }
      return make_error(ErrorCode::invalid_argument, "expected ',' or '}' in json object",
                        "offset " + std::to_string(pos_));
    }
  }

  Status parse_hex4(std::uint32_t& out) {
    if (pos_ + 4 > text_.size()) {
      return make_error(ErrorCode::invalid_argument, "truncated json unicode escape");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + static_cast<std::size_t>(i)];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A' + 10);
      else return make_error(ErrorCode::invalid_argument, "invalid hex digit in json escape");
      value = (value << 4) | digit;
    }
    pos_ += 4;
    out = value;
    return ok_status();
  }

  static void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7Fu) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFu) {
      out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point <= 0xFFFFu) {
      out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
  }

  Status parse_string(std::string& out) {
    ++pos_;  // opening quote
    std::string result;
    for (;;) {
      if (at_end()) {
        return make_error(ErrorCode::invalid_argument, "unterminated json string");
      }
      const char c = text_[pos_];
      if (c == '"') {
        ++pos_;
        out = std::move(result);
        return ok_status();
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return make_error(ErrorCode::invalid_argument,
                          "unescaped control character in json string",
                          "offset " + std::to_string(pos_));
      }
      if (c == '\\') {
        ++pos_;
        if (at_end()) {
          return make_error(ErrorCode::invalid_argument, "truncated json escape");
        }
        const char escape = text_[pos_++];
        switch (escape) {
          case '"': result.push_back('"'); break;
          case '\\': result.push_back('\\'); break;
          case '/': result.push_back('/'); break;
          case 'b': result.push_back('\b'); break;
          case 'f': result.push_back('\f'); break;
          case 'n': result.push_back('\n'); break;
          case 'r': result.push_back('\r'); break;
          case 't': result.push_back('\t'); break;
          case 'u': {
            std::uint32_t code_point = 0;
            FUM_TRYV(parse_hex4(code_point));
            if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
              if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                pos_ += 2;
                std::uint32_t low = 0;
                FUM_TRYV(parse_hex4(low));
                if (low < 0xDC00u || low > 0xDFFFu) {
                  return make_error(ErrorCode::invalid_argument,
                                    "invalid json surrogate pair");
                }
                code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
              } else {
                return make_error(ErrorCode::invalid_argument, "lone json high surrogate");
              }
            } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
              return make_error(ErrorCode::invalid_argument, "lone json low surrogate");
            }
            append_utf8(result, code_point);
            break;
          }
          default:
            return make_error(ErrorCode::invalid_argument, "invalid json escape character");
        }
        continue;
      }
      result.push_back(c);
      ++pos_;
      if (result.size() > limits_.max_string_bytes) {
        return make_error(ErrorCode::resource_exhausted, "json string exceeds the size bound");
      }
    }
  }

  Status parse_number(Value& out) {
    const std::size_t start = pos_;
    if (!at_end() && peek() == '-') {
      ++pos_;
    }
    if (at_end() || peek() < '0' || peek() > '9') {
      return make_error(ErrorCode::invalid_argument, "invalid json number",
                        "offset " + std::to_string(start));
    }
    if (peek() == '0') {
      ++pos_;
      if (!at_end() && peek() >= '0' && peek() <= '9') {
        return make_error(ErrorCode::invalid_argument, "json number has a leading zero",
                          "offset " + std::to_string(start));
      }
    } else {
      while (!at_end() && peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }
    bool integral = true;
    if (!at_end() && peek() == '.') {
      integral = false;
      ++pos_;
      if (at_end() || peek() < '0' || peek() > '9') {
        return make_error(ErrorCode::invalid_argument, "json number has an empty fraction");
      }
      while (!at_end() && peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }
    if (!at_end() && (peek() == 'e' || peek() == 'E')) {
      integral = false;
      ++pos_;
      if (!at_end() && (peek() == '+' || peek() == '-')) {
        ++pos_;
      }
      if (at_end() || peek() < '0' || peek() > '9') {
        return make_error(ErrorCode::invalid_argument, "json number has an empty exponent");
      }
      while (!at_end() && peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }
    const std::string_view token = text_.substr(start, pos_ - start);
    if (integral) {
      std::int64_t signed_value = 0;
      const auto signed_result =
          std::from_chars(token.data(), token.data() + token.size(), signed_value);
      if (signed_result.ec == std::errc{} &&
          signed_result.ptr == token.data() + token.size()) {
        out = Value::make_int(signed_value);
        return ok_status();
      }
      if (!token.empty() && token.front() != '-') {
        std::uint64_t unsigned_value = 0;
        const auto unsigned_result =
            std::from_chars(token.data(), token.data() + token.size(), unsigned_value);
        if (unsigned_result.ec == std::errc{} &&
            unsigned_result.ptr == token.data() + token.size()) {
          out = Value::make_uint(unsigned_value);
          return ok_status();
        }
      }
    }
    double real_value = 0.0;
    const auto real_result =
        std::from_chars(token.data(), token.data() + token.size(), real_value);
    if (real_result.ec != std::errc{} || real_result.ptr != token.data() + token.size()) {
      return make_error(ErrorCode::invalid_argument, "json number could not be represented",
                        std::string(token));
    }
    if (!std::isfinite(real_value)) {
      return make_error(ErrorCode::invalid_argument, "json number is not finite",
                        std::string(token));
    }
    out = Value::make_real(real_value);
    return ok_status();
  }

  std::string_view text_;
  Limits limits_;
  std::size_t pos_ = 0;
  std::size_t nodes_ = 0;
};

}  // namespace

Value Value::make_bool(bool v) {
  Value value;
  value.kind_ = Kind::boolean;
  value.bool_ = v;
  return value;
}

Value Value::make_int(std::int64_t v) {
  Value value;
  value.kind_ = Kind::integer;
  value.int_ = v;
  return value;
}

Value Value::make_uint(std::uint64_t v) {
  Value value;
  value.kind_ = Kind::unsigned_integer;
  value.uint_ = v;
  return value;
}

Value Value::make_real(double v) {
  Value value;
  value.kind_ = Kind::real;
  value.real_ = v;
  return value;
}

Value Value::make_string(std::string v) {
  Value value;
  value.kind_ = Kind::string;
  value.string_ = std::move(v);
  return value;
}

Value Value::make_array() {
  Value value;
  value.kind_ = Kind::array;
  return value;
}

Value Value::make_object() {
  Value value;
  value.kind_ = Kind::object;
  return value;
}

Result<bool> Value::as_bool() const {
  if (kind_ != Kind::boolean) {
    return make_error(ErrorCode::invalid_argument, "json value is not a boolean");
  }
  return bool_;
}

Result<std::int64_t> Value::as_int() const {
  switch (kind_) {
    case Kind::integer: return int_;
    case Kind::unsigned_integer:
      if (uint_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return make_error(ErrorCode::invalid_argument, "json integer does not fit in int64");
      }
      return static_cast<std::int64_t>(uint_);
    case Kind::real:
      if (real_ != std::floor(real_) || std::abs(real_) > 9.2e18) {
        return make_error(ErrorCode::invalid_argument, "json number is not an integer");
      }
      return static_cast<std::int64_t>(real_);
    default:
      return make_error(ErrorCode::invalid_argument, "json value is not a number");
  }
}

Result<std::uint64_t> Value::as_uint() const {
  switch (kind_) {
    case Kind::unsigned_integer: return uint_;
    case Kind::integer:
      if (int_ < 0) {
        return make_error(ErrorCode::invalid_argument, "json integer is negative");
      }
      return static_cast<std::uint64_t>(int_);
    case Kind::real:
      if (real_ != std::floor(real_) || real_ < 0.0 || real_ > 1.8e19) {
        return make_error(ErrorCode::invalid_argument, "json number is not an unsigned integer");
      }
      return static_cast<std::uint64_t>(real_);
    default:
      return make_error(ErrorCode::invalid_argument, "json value is not a number");
  }
}

Result<double> Value::as_double() const {
  switch (kind_) {
    case Kind::real: return real_;
    case Kind::integer: return static_cast<double>(int_);
    case Kind::unsigned_integer: return static_cast<double>(uint_);
    default: return make_error(ErrorCode::invalid_argument, "json value is not a number");
  }
}

Result<std::string_view> Value::as_string() const {
  if (kind_ != Kind::string) {
    return make_error(ErrorCode::invalid_argument, "json value is not a string");
  }
  return std::string_view(string_);
}

const Value::Array& Value::items() const {
  static const Array kEmpty;
  return kind_ == Kind::array ? array_ : kEmpty;
}

const Value::Object& Value::members() const {
  static const Object kEmpty;
  return kind_ == Kind::object ? object_ : kEmpty;
}

const Value* Value::find(std::string_view key) const {
  if (kind_ != Kind::object) {
    return nullptr;
  }
  for (const auto& member : object_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

Result<std::string_view> Value::require_string(std::string_view key) const {
  const Value* found = find(key);
  if (found == nullptr) {
    return make_error(ErrorCode::invalid_argument, "required json member is missing",
                      std::string(key));
  }
  return found->as_string();
}

Result<std::uint64_t> Value::require_uint(std::string_view key) const {
  const Value* found = find(key);
  if (found == nullptr) {
    return make_error(ErrorCode::invalid_argument, "required json member is missing",
                      std::string(key));
  }
  return found->as_uint();
}

Result<bool> Value::require_bool(std::string_view key) const {
  const Value* found = find(key);
  if (found == nullptr) {
    return make_error(ErrorCode::invalid_argument, "required json member is missing",
                      std::string(key));
  }
  return found->as_bool();
}

Result<double> Value::require_double(std::string_view key) const {
  const Value* found = find(key);
  if (found == nullptr) {
    return make_error(ErrorCode::invalid_argument, "required json member is missing",
                      std::string(key));
  }
  return found->as_double();
}

void Value::set(std::string key, Value value) {
  if (kind_ != Kind::object) {
    kind_ = Kind::object;
    object_.clear();
  }
  for (auto& member : object_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

void Value::erase(std::string_view key) {
  if (kind_ != Kind::object) {
    return;
  }
  for (auto it = object_.begin(); it != object_.end(); ++it) {
    if (it->first == key) {
      object_.erase(it);
      return;
    }
  }
}

void Value::push(Value value) {
  if (kind_ != Kind::array) {
    kind_ = Kind::array;
    array_.clear();
  }
  array_.push_back(std::move(value));
}

void Value::dump_into(std::string& out, int indent, int depth) const {
  const bool pretty = indent > 0;
  const std::string pad = pretty ? std::string(static_cast<std::size_t>(indent * (depth + 1)), ' ') : std::string();
  const std::string pad_close =
      pretty ? std::string(static_cast<std::size_t>(indent * depth), ' ') : std::string();
  switch (kind_) {
    case Kind::null_value: out.append("null"); break;
    case Kind::boolean: out.append(bool_ ? "true" : "false"); break;
    case Kind::integer: out.append(std::to_string(int_)); break;
    case Kind::unsigned_integer: out.append(std::to_string(uint_)); break;
    case Kind::real: append_real(out, real_); break;
    case Kind::string: append_escaped(out, string_); break;
    case Kind::array: {
      if (array_.empty()) {
        out.append("[]");
        break;
      }
      out.push_back('[');
      bool first = true;
      for (const auto& item : array_) {
        if (!first) out.push_back(',');
        first = false;
        if (pretty) {
          out.push_back('\n');
          out.append(pad);
        }
        item.dump_into(out, indent, depth + 1);
      }
      if (pretty) {
        out.push_back('\n');
        out.append(pad_close);
      }
      out.push_back(']');
      break;
    }
    case Kind::object: {
      if (object_.empty()) {
        out.append("{}");
        break;
      }
      out.push_back('{');
      bool first = true;
      for (const auto& member : object_) {
        if (!first) out.push_back(',');
        first = false;
        if (pretty) {
          out.push_back('\n');
          out.append(pad);
        }
        append_escaped(out, member.first);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        member.second.dump_into(out, indent, depth + 1);
      }
      if (pretty) {
        out.push_back('\n');
        out.append(pad_close);
      }
      out.push_back('}');
      break;
    }
  }
}

std::string Value::dump() const {
  std::string out;
  dump_into(out, 0, 0);
  return out;
}

std::string Value::dump_pretty(int indent) const {
  std::string out;
  dump_into(out, indent, 0);
  return out;
}

bool operator==(const Value& a, const Value& b) {
  if (a.kind_ != b.kind_) {
    const bool both_numeric = a.is_number() && b.is_number();
    if (!both_numeric) {
      return false;
    }
    const auto left = a.as_double();
    const auto right = b.as_double();
    return left.has_value() && right.has_value() && left.value() == right.value();
  }
  switch (a.kind_) {
    case Value::Kind::null_value: return true;
    case Value::Kind::boolean: return a.bool_ == b.bool_;
    case Value::Kind::integer: return a.int_ == b.int_;
    case Value::Kind::unsigned_integer: return a.uint_ == b.uint_;
    case Value::Kind::real: return a.real_ == b.real_;
    case Value::Kind::string: return a.string_ == b.string_;
    case Value::Kind::array: {
      if (a.array_.size() != b.array_.size()) return false;
      for (std::size_t i = 0; i < a.array_.size(); ++i) {
        if (!(a.array_[i] == b.array_[i])) return false;
      }
      return true;
    }
    case Value::Kind::object: {
      if (a.object_.size() != b.object_.size()) return false;
      for (std::size_t i = 0; i < a.object_.size(); ++i) {
        if (a.object_[i].first != b.object_[i].first) return false;
        if (!(a.object_[i].second == b.object_[i].second)) return false;
      }
      return true;
    }
  }
  return false;
}

Result<Value> parse(std::string_view text, const Limits& limits) {
  Parser parser(text, limits);
  return parser.run();
}

Result<Value> parse_object(std::string_view text, const Limits& limits) {
  auto parsed = parse(text, limits);
  if (!parsed.has_value()) {
    return parsed.error();
  }
  if (!parsed.value().is_object()) {
    return make_error(ErrorCode::invalid_argument, "json document root is not an object");
  }
  return std::move(parsed).value();
}

}  // namespace fum::json
