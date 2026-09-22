// Real framed transport over TCP loopback sockets.
//
// Frame: u32 big-endian payload length | u32 big-endian crc32c(payload) | payload
// Frames are bounded on both sides: an oversized or corrupt frame is rejected
// and the connection is closed, never partially trusted.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "fum/core/result.hpp"

namespace fum::net {

inline constexpr std::uint32_t kMaxFrameBytes = 1024u * 1024u;

// Winsock lifetime: safe to call repeatedly, required before any socket use.
[[nodiscard]] Status ensure_socket_runtime();

class [[nodiscard]] TcpConnection {
 public:
  TcpConnection() = default;
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&& other) noexcept;
  TcpConnection& operator=(TcpConnection&& other) noexcept;
  ~TcpConnection();

  [[nodiscard]] static Result<TcpConnection> connect_loopback(std::uint16_t port);

  [[nodiscard]] Status send_message(std::string_view payload);
  [[nodiscard]] Result<std::string> receive_message(std::uint32_t max_bytes = kMaxFrameBytes);

  void close();
  [[nodiscard]] bool valid() const noexcept { return handle_ >= 0; }
  [[nodiscard]] int handle() const noexcept { return handle_; }
  int release() noexcept;

 private:
  friend class TcpListener;
  explicit TcpConnection(int handle) : handle_(handle) {}
  int handle_ = -1;
};

class [[nodiscard]] TcpListener {
 public:
  TcpListener() = default;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  ~TcpListener();

  // Binds to 127.0.0.1; port 0 selects an ephemeral port.
  [[nodiscard]] static Result<TcpListener> bind_loopback(std::uint16_t port);
  [[nodiscard]] Result<TcpConnection> accept_one();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  void close();
  [[nodiscard]] bool valid() const noexcept { return handle_ >= 0; }

 private:
  int handle_ = -1;
  std::uint16_t port_ = 0;
};

}  // namespace fum::net
