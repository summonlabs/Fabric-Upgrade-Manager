// Framed request/response protocol between the local-process adapter and the
// control-plane executable it manages. Internal to the adapter implementation.
#pragma once

#include <cstdint>
#include <string>

#include "fum/adapters/local_process/framed_transport.hpp"
#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/result.hpp"

namespace fum::lp {

inline constexpr const char* kProtocolVersion = "1";

[[nodiscard]] inline json::Value make_request(const std::string& command, Sequence request_id,
                                              Incarnation incarnation,
                                              const std::string& token) {
  json::Value request = json::Value::make_object();
  request.set("protocol", json::Value::make_string(kProtocolVersion));
  request.set("command", json::Value::make_string(command));
  request.set("request_id", json::Value::make_uint(request_id.value()));
  request.set("incarnation", json::Value::make_uint(incarnation.value()));
  request.set("token", json::Value::make_string(token));
  return request;
}

[[nodiscard]] inline Result<json::Value> send_command(net::TcpConnection& connection,
                                                      const json::Value& request) {
  const std::string encoded = request.dump();
  FUM_TRYV(connection.send_message(encoded));
  auto response = connection.receive_message();
  if (!response.has_value()) {
    return response.error();
  }
  return json::parse(response.value());
}

[[nodiscard]] inline Result<json::Value> decode_request(const std::string& payload) {
  return json::parse_object(payload);
}

[[nodiscard]] inline std::string encode_response(const json::Value& response) {
  return response.dump();
}

}  // namespace fum::lp
