#include "fum/adapters/local_process/framed_transport.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <thread>

#include "fum/core/crc.hpp"
#include "fum/core/time.hpp"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fum::net {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalid = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalid = -1;
#endif

std::once_flag g_socket_once;
Status g_socket_status = ok_status();

void initialize_sockets() {
#if defined(_WIN32)
  WSADATA data;
  const int result = WSAStartup(MAKEWORD(2, 2), &data);
  if (result != 0) {
    g_socket_status = make_error(ErrorCode::io_error, "WSAStartup failed",
                                 std::to_string(result));
  }
#else
  g_socket_status = ok_status();
#endif
}

Status socket_status() {
  std::call_once(g_socket_once, initialize_sockets);
  return g_socket_status;
}

void close_handle(SocketHandle handle) {
  if (handle == kInvalid) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(handle);
#else
  ::close(handle);
#endif
}

Status send_all(SocketHandle handle, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const int chunk = static_cast<int>(
        std::min<std::size_t>(data.size() - sent, static_cast<std::size_t>(1) << 20));
#if defined(_WIN32)
    const int result = ::send(handle, data.data() + sent, chunk, 0);
#else
    const int result = static_cast<int>(::send(handle, data.data() + sent,
                                               static_cast<std::size_t>(chunk), 0));
#endif
    if (result <= 0) {
      return make_error(ErrorCode::io_error, "socket send failed");
    }
    sent += static_cast<std::size_t>(result);
  }
  return ok_status();
}

Result<bool> receive_exact(SocketHandle handle, char* buffer, std::size_t bytes) {
  std::size_t received = 0;
  while (received < bytes) {
#if defined(_WIN32)
    const int result = ::recv(handle, buffer + received,
                              static_cast<int>(bytes - received), 0);
#else
    const int result = static_cast<int>(::recv(handle, buffer + received, bytes - received, 0));
#endif
    if (result == 0) {
      return false;   // peer closed
    }
    if (result < 0) {
      return make_error(ErrorCode::io_error, "socket receive failed");
    }
    received += static_cast<std::size_t>(result);
  }
  return true;
}

std::string encode_frame(std::string_view payload) {
  std::string frame;
  frame.reserve(payload.size() + 8);
  const auto length = static_cast<std::uint32_t>(payload.size());
  frame.push_back(static_cast<char>((length >> 24) & 0xFFu));
  frame.push_back(static_cast<char>((length >> 16) & 0xFFu));
  frame.push_back(static_cast<char>((length >> 8) & 0xFFu));
  frame.push_back(static_cast<char>(length & 0xFFu));
  const std::uint32_t crc = crc::crc32c(payload);
  frame.push_back(static_cast<char>((crc >> 24) & 0xFFu));
  frame.push_back(static_cast<char>((crc >> 16) & 0xFFu));
  frame.push_back(static_cast<char>((crc >> 8) & 0xFFu));
  frame.push_back(static_cast<char>(crc & 0xFFu));
  frame.append(payload);
  return frame;
}

}  // namespace

Status ensure_socket_runtime() { return socket_status(); }

TcpConnection::TcpConnection(TcpConnection&& other) noexcept : handle_(other.handle_) {
  other.handle_ = -1;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = -1;
  }
  return *this;
}

TcpConnection::~TcpConnection() { close(); }

void TcpConnection::close() {
  if (handle_ >= 0) {
    close_handle(static_cast<SocketHandle>(handle_));
    handle_ = -1;
  }
}

int TcpConnection::release() noexcept {
  const int handle = handle_;
  handle_ = -1;
  return handle;
}

Result<TcpConnection> TcpConnection::connect_loopback(std::uint16_t port) {
  FUM_TRYV(ensure_socket_runtime());
  const SocketHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalid) {
    return make_error(ErrorCode::io_error, "could not create a socket");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_handle(handle);
    return make_error(ErrorCode::io_error, "could not connect to the loopback endpoint",
                      std::to_string(port));
  }
  const int one = 1;
  static_cast<void>(::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&one), sizeof(one)));
  return TcpConnection(static_cast<int>(handle));
}

Status TcpConnection::send_message(std::string_view payload) {
  if (handle_ < 0) {
    return make_error(ErrorCode::closed, "the connection is closed");
  }
  if (payload.size() > kMaxFrameBytes) {
    return make_error(ErrorCode::resource_exhausted, "frame exceeds the transport bound",
                      std::to_string(payload.size()));
  }
  const std::string frame = encode_frame(payload);
  return send_all(static_cast<SocketHandle>(handle_), frame);
}

Result<std::string> TcpConnection::receive_message(std::uint32_t max_bytes) {
  if (handle_ < 0) {
    return make_error(ErrorCode::closed, "the connection is closed");
  }
  std::array<char, 8> header{};
  auto header_result = receive_exact(static_cast<SocketHandle>(handle_), header.data(), header.size());
  if (!header_result.has_value()) {
    return header_result.error();
  }
  if (!header_result.value()) {
    return make_error(ErrorCode::closed, "the peer closed the connection");
  }
  const auto length = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
                      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
                      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
                      static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
  const auto crc = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[4])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(header[5])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(header[6])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(header[7]));
  const std::uint32_t limit = std::min(max_bytes, kMaxFrameBytes);
  if (length > limit) {
    close();
    return make_error(ErrorCode::resource_exhausted, "frame exceeds the permitted bound",
                      std::to_string(length) + " > " + std::to_string(limit));
  }
  std::string payload;
  payload.resize(length);
  if (length > 0) {
    auto payload_result =
        receive_exact(static_cast<SocketHandle>(handle_), payload.data(), payload.size());
    if (!payload_result.has_value()) {
      return payload_result.error();
    }
    if (!payload_result.value()) {
      close();
      return make_error(ErrorCode::io_error, "the peer closed mid-frame");
    }
  }
  if (crc::crc32c(payload) != crc) {
    close();
    return make_error(ErrorCode::integrity_failure, "frame checksum mismatch");
  }
  return payload;
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), port_(other.port_) {
  other.handle_ = -1;
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = -1;
    other.port_ = 0;
  }
  return *this;
}

TcpListener::~TcpListener() { close(); }

void TcpListener::close() {
  if (handle_ >= 0) {
    close_handle(static_cast<SocketHandle>(handle_));
    handle_ = -1;
  }
}

Result<TcpListener> TcpListener::bind_loopback(std::uint16_t port) {
  FUM_TRYV(ensure_socket_runtime());
  const SocketHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalid) {
    return make_error(ErrorCode::io_error, "could not create a listener socket");
  }
  const int one = 1;
  static_cast<void>(::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                                 reinterpret_cast<const char*>(&one), sizeof(one)));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_handle(handle);
    return make_error(ErrorCode::io_error, "could not bind the loopback endpoint",
                      std::to_string(port));
  }
  if (::listen(handle, 4) != 0) {
    close_handle(handle);
    return make_error(ErrorCode::io_error, "could not listen on the loopback endpoint");
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int bound_length = sizeof(bound);
#else
  socklen_t bound_length = sizeof(bound);
#endif
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    close_handle(handle);
    return make_error(ErrorCode::io_error, "could not read the bound endpoint");
  }
  TcpListener listener;
  listener.handle_ = static_cast<int>(handle);
  listener.port_ = ntohs(bound.sin_port);
  return listener;
}

Result<TcpConnection> TcpListener::accept_one() {
  if (handle_ < 0) {
    return make_error(ErrorCode::closed, "the listener is closed");
  }
  sockaddr_in peer{};
#if defined(_WIN32)
  int peer_length = sizeof(peer);
#else
  socklen_t peer_length = sizeof(peer);
#endif
  const SocketHandle accepted =
      ::accept(static_cast<SocketHandle>(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
  if (accepted == kInvalid) {
    return make_error(ErrorCode::io_error, "accept failed");
  }
  const int one = 1;
  static_cast<void>(::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&one), sizeof(one)));
  return TcpConnection(static_cast<int>(accepted));
}

}  // namespace fum::net
