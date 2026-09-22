// fum_test_control_plane: a real control-plane executable used by the
// local-process upgrade adapter and by its independent-process tests.
//
// It reads the package descriptor installed next to its own image, serves a
// framed request/response protocol over a loopback socket, and enforces
// incarnation fencing on every command. It claims nothing about vendor
// hardware: it is a test control plane and says so.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "fum/adapters/local_process/framed_transport.hpp"
#include "fum/core/fs.hpp"
#include "fum/core/json.hpp"
#include "fum/core/time.hpp"

#include "control_plane_protocol.hpp"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

struct Options {
  std::string state_directory;
  std::uint16_t port = 0;
  std::string token;
  std::string target;
};

struct Package {
  std::string version = "0.0.0";
  std::string build = "unknown";
  std::string component = "unknown";
  bool healthy = true;
  std::int64_t startup_delay_ms = 0;
  bool crash_on_start = false;
};

std::string executable_directory(const char* argv0) {
#if defined(_WIN32)
  char buffer[MAX_PATH];
  const DWORD length = ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    std::string path(buffer, length);
    const std::size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) {
      return path.substr(0, slash);
    }
  }
#else
  static_cast<void>(argv0);
#endif
  return fum::fs::parent_directory(argv0 == nullptr ? "." : argv0);
}

fum::Result<Options> parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool has_next = i + 1 < argc;
    if (argument == "--state" && has_next) {
      options.state_directory = argv[++i];
    } else if (argument == "--port" && has_next) {
      options.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (argument == "--token" && has_next) {
      options.token = argv[++i];
    } else if (argument == "--target" && has_next) {
      options.target = argv[++i];
    } else {
      return fum::make_error(fum::ErrorCode::invalid_argument,
                             "unrecognized control plane argument", argument);
    }
  }
  if (options.state_directory.empty()) {
    return fum::make_error(fum::ErrorCode::invalid_argument, "--state is required");
  }
  return options;
}

fum::Result<Package> load_package(const std::string& directory) {
  const std::string path = fum::fs::join(directory, "package.json");
  if (!fum::fs::exists(path)) {
    return fum::make_error(fum::ErrorCode::not_found, "no package descriptor is installed", path);
  }
  auto text = fum::fs::read_file(path, 256 * 1024);
  if (!text.has_value()) {
    return text.error();
  }
  auto parsed = fum::json::parse_object(text.value());
  if (!parsed.has_value()) {
    return parsed.error();
  }
  Package package;
  std::string_view version;
  if (auto result = parsed.value().require_string("version"); result.has_value()) {
    version = result.value();
    package.version = std::string(version);
  }
  if (auto result = parsed.value().require_string("build"); result.has_value()) {
    package.build = std::string(result.value());
  }
  if (auto result = parsed.value().require_string("component"); result.has_value()) {
    package.component = std::string(result.value());
  }
  if (const fum::json::Value* behavior = parsed.value().find("behavior"); behavior != nullptr) {
    if (const fum::json::Value* healthy = behavior->find("healthy"); healthy != nullptr) {
      package.healthy = healthy->as_bool().value_or(true);
    }
    if (const fum::json::Value* delay = behavior->find("startup_delay_ms"); delay != nullptr) {
      package.startup_delay_ms = delay->as_int().value_or(0);
    }
    if (const fum::json::Value* crash = behavior->find("crash_on_start"); crash != nullptr) {
      package.crash_on_start = crash->as_bool().value_or(false);
    }
  }
  return package;
}

std::uint64_t fresh_incarnation() {
  std::random_device device;
  std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  const auto now = static_cast<std::uint64_t>(fum::SystemClock::instance().now().unix_nanos());
  value ^= now;
#if defined(_WIN32)
  value ^= static_cast<std::uint64_t>(::GetCurrentProcessId()) << 17;
#else
  value ^= static_cast<std::uint64_t>(::getpid()) << 17;
#endif
  return value == 0 ? 1 : value;
}

fum::json::Value error_response(std::string message) {
  fum::json::Value response = fum::json::Value::make_object();
  response.set("ok", fum::json::Value::make_bool(false));
  response.set("error", fum::json::Value::make_string(std::move(message)));
  return response;
}

}  // namespace

int main(int argc, char** argv) {
  auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    std::fprintf(stderr, "control plane: %s\n", options.error().to_string().c_str());
    return 2;
  }
  const std::string directory = executable_directory(argc > 0 ? argv[0] : nullptr);
  auto package = load_package(directory);
  if (!package.has_value()) {
    std::fprintf(stderr, "control plane: %s\n", package.error().to_string().c_str());
    return 2;
  }
  const std::uint64_t incarnation = fresh_incarnation();
  const std::string state_directory = options.value().state_directory;
  if (!fum::fs::ensure_directory(state_directory).has_value()) {
    std::fprintf(stderr, "control plane: could not create the state directory\n");
    return 2;
  }
  if (package.value().crash_on_start) {
    std::fprintf(stderr, "control plane: crashing on start as instructed by the package\n");
    return 3;
  }
  if (package.value().startup_delay_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(package.value().startup_delay_ms));
  }

  auto listener = fum::net::TcpListener::bind_loopback(options.value().port);
  if (!listener.has_value()) {
    std::fprintf(stderr, "control plane: %s\n", listener.error().to_string().c_str());
    return 2;
  }
  const std::uint16_t port = listener.value().port();

  fum::json::Value runtime = fum::json::Value::make_object();
  runtime.set("pid", fum::json::Value::make_uint(static_cast<std::uint64_t>(
#if defined(_WIN32)
                         ::GetCurrentProcessId()
#else
                         ::getpid()
#endif
                         )));
  runtime.set("incarnation", fum::json::Value::make_uint(incarnation));
  runtime.set("version", fum::json::Value::make_string(package.value().version));
  runtime.set("build", fum::json::Value::make_string(package.value().build));
  runtime.set("component", fum::json::Value::make_string(package.value().component));
  runtime.set("target", fum::json::Value::make_string(options.value().target));
  runtime.set("port", fum::json::Value::make_uint(port));
  runtime.set("started_at", fum::json::Value::make_string(fum::SystemClock::instance().now().to_iso8601()));
  static_cast<void>(
      fum::fs::write_file_atomic(fum::fs::join(state_directory, "runtime.json"), runtime.dump()));

  std::printf("READY %u %llu\n", static_cast<unsigned>(port),
              static_cast<unsigned long long>(incarnation));
  std::fflush(stdout);

  bool running = true;
  while (running) {
    auto connection = listener.value().accept_one();
    if (!connection.has_value()) {
      continue;
    }
    for (;;) {
      auto payload = connection.value().receive_message();
      if (!payload.has_value()) {
        break;   // malformed frame or closed connection: drop it and keep serving
      }
      auto request = fum::lp::decode_request(payload.value());
      if (!request.has_value()) {
        const std::string encoded =
            fum::lp::encode_response(error_response("malformed request document"));
        static_cast<void>(connection.value().send_message(encoded));
        continue;
      }
      std::string_view command;
      if (auto result = request.value().require_string("command"); result.has_value()) {
        command = result.value();
      } else {
        static_cast<void>(connection.value().send_message(
            fum::lp::encode_response(error_response("request has no command"))));
        continue;
      }
      const std::uint64_t expected =
          request.value().require_uint("incarnation").value_or(incarnation);
      const std::string token =
          std::string(request.value().require_string("token").value_or(""));

      if (command == "crash") {
        std::fprintf(stderr, "control plane: crashing as instructed\n");
        std::fflush(stderr);
        std::_Exit(9);
      }
      if (command == "shutdown") {
        const std::string encoded =
            fum::lp::encode_response([] {
              fum::json::Value response = fum::json::Value::make_object();
              response.set("ok", fum::json::Value::make_bool(true));
              response.set("detail", fum::json::Value::make_string("shutting down"));
              return response;
            }());
        static_cast<void>(connection.value().send_message(encoded));
        running = false;
        break;
      }
      if (!options.value().token.empty() && token != options.value().token) {
        static_cast<void>(connection.value().send_message(
            fum::lp::encode_response(error_response("token mismatch"))));
        continue;
      }
      if (expected != incarnation) {
        static_cast<void>(connection.value().send_message(fum::lp::encode_response(
            error_response("fenced: command belongs to incarnation " + std::to_string(expected) +
                           " but this process is incarnation " + std::to_string(incarnation)))));
        continue;
      }

      fum::json::Value response = fum::json::Value::make_object();
      response.set("ok", fum::json::Value::make_bool(true));
      response.set("command", fum::json::Value::make_string(std::string(command)));
      response.set("incarnation", fum::json::Value::make_uint(incarnation));
      response.set("version", fum::json::Value::make_string(package.value().version));
      response.set("build", fum::json::Value::make_string(package.value().build));
      response.set("healthy", fum::json::Value::make_bool(package.value().healthy));
      response.set("pid", fum::json::Value::make_string(std::to_string(
#if defined(_WIN32)
                             ::GetCurrentProcessId()
#else
                             ::getpid()
#endif
                             )));
      response.set("protocol", fum::json::Value::make_string(fum::lp::kProtocolVersion));
      static_cast<void>(connection.value().send_message(fum::lp::encode_response(response)));
    }
    connection.value().close();
  }
  static_cast<void>(listener.value().close());
  return 0;
}
