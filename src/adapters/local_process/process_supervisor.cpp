#include "fum/adapters/local_process/process_supervisor.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fum::proc {
namespace {

std::string quote_argument(const std::string& argument) {
  if (argument.find_first_of(" \"\t\"") == std::string::npos) {
    return argument;
  }
  std::string out = "\"";
  for (const char c : argument) {
    if (c == '"') {
      out.append("\\\"");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace

ProcessSupervisor::ProcessSupervisor(ProcessSupervisor&& other) noexcept
    : pid_(other.pid_),
      process_handle_(other.process_handle_),
      thread_handle_(other.thread_handle_),
      read_handle_(other.read_handle_),
      write_handle_(other.write_handle_),
      exit_code_(other.exit_code_),
      exited_(other.exited_),
      captured_(std::move(other.captured_)) {
  other.pid_ = 0;
  other.process_handle_ = nullptr;
  other.thread_handle_ = nullptr;
  other.read_handle_ = nullptr;
  other.write_handle_ = nullptr;
}

ProcessSupervisor& ProcessSupervisor::operator=(ProcessSupervisor&& other) noexcept {
  if (this != &other) {
    static_cast<void>(kill());
    close_handles();
    pid_ = other.pid_;
    process_handle_ = other.process_handle_;
    thread_handle_ = other.thread_handle_;
    read_handle_ = other.read_handle_;
    write_handle_ = other.write_handle_;
    exit_code_ = other.exit_code_;
    exited_ = other.exited_;
    captured_ = std::move(other.captured_);
    other.pid_ = 0;
    other.process_handle_ = nullptr;
    other.thread_handle_ = nullptr;
    other.read_handle_ = nullptr;
    other.write_handle_ = nullptr;
  }
  return *this;
}

ProcessSupervisor::~ProcessSupervisor() {
  static_cast<void>(kill());
  close_handles();
}

Result<ProcessSupervisor> ProcessSupervisor::create() {
#if defined(_WIN32)
  return ProcessSupervisor{};
#else
  return make_error(ErrorCode::unsupported,
                    "the local-process adapter currently supports Windows hosts only");
#endif
}

void ProcessSupervisor::close_handles() {
#if defined(_WIN32)
  if (read_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(read_handle_));
    read_handle_ = nullptr;
  }
  if (write_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(write_handle_));
    write_handle_ = nullptr;
  }
  if (thread_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(thread_handle_));
    thread_handle_ = nullptr;
  }
  if (process_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_handle_));
    process_handle_ = nullptr;
  }
#endif
  pid_ = 0;
}

Status ProcessSupervisor::start(const StartRequest& request) {
#if defined(_WIN32)
  close_handles();
  exited_ = false;
  exit_code_ = -1;
  captured_.clear();

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (!::CreatePipe(&read_handle, &write_handle, &attributes, 0)) {
    return make_error(ErrorCode::io_error, "could not create the child output pipe");
  }
  if (!::SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0)) {
    ::CloseHandle(read_handle);
    ::CloseHandle(write_handle);
    return make_error(ErrorCode::io_error, "could not configure the child output pipe");
  }

  std::string command_line = quote_argument(request.executable);
  for (const auto& argument : request.arguments) {
    command_line.push_back(' ');
    command_line.append(quote_argument(argument));
  }
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_handle;
  startup.hStdError = write_handle;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION information{};
  const char* working_directory =
      request.working_directory.empty() ? nullptr : request.working_directory.c_str();
  const BOOL created = ::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, working_directory, &startup,
                                        &information);
  ::CloseHandle(write_handle);
  if (!created) {
    ::CloseHandle(read_handle);
    return make_error(ErrorCode::io_error, "could not start the target process",
                      request.executable + " (error " + std::to_string(::GetLastError()) + ")");
  }
  read_handle_ = read_handle;
  process_handle_ = information.hProcess;
  thread_handle_ = information.hThread;
  pid_ = static_cast<std::uint64_t>(information.dwProcessId);
  return ok_status();
#else
  static_cast<void>(request);
  return make_error(ErrorCode::unsupported, "the local-process adapter requires Windows");
#endif
}

Result<StartedProcess> ProcessSupervisor::await_ready(const StartRequest& request) {
#if defined(_WIN32)
  if (process_handle_ == nullptr || read_handle_ == nullptr) {
    return make_error(ErrorCode::precondition_failed, "no child process is running");
  }
  StartedProcess result;
  result.pid = pid_;
  const Timestamp start = SystemClock::instance().now();
  std::string buffer;
  for (;;) {
    DWORD available = 0;
    if (::PeekNamedPipe(static_cast<HANDLE>(read_handle_), nullptr, 0, nullptr, &available,
                        nullptr)) {
      if (available > 0) {
        char chunk[512];
        DWORD read = 0;
        const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));
        if (::ReadFile(static_cast<HANDLE>(read_handle_), chunk, want, &read, nullptr) && read > 0) {
          buffer.append(chunk, read);
          captured_.append(chunk, read);
          for (;;) {
            const std::size_t newline = buffer.find('\n');
            if (newline == std::string::npos) {
              break;
            }
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
              line.pop_back();
            }
            if (line.rfind(request.ready_prefix, 0) == 0) {
              result.ready_line = line;
              result.trailing_output = buffer;
              return result;
            }
            if (!line.empty()) {
              result.trailing_output.append(line);
              result.trailing_output.push_back('\n');
            }
          }
        }
      } else {
        const DWORD wait = ::WaitForSingleObject(process_handle_, 0);
        if (wait == WAIT_OBJECT_0) {
          DWORD code = 0;
          ::GetExitCodeProcess(process_handle_, &code);
          exit_code_ = static_cast<int>(code);
          exited_ = true;
          return make_error(ErrorCode::io_error,
                            "the target process exited before reporting readiness",
                            "exit code " + std::to_string(exit_code_) + "; output: " + captured_);
        }
      }
    } else {
      return make_error(ErrorCode::io_error, "could not poll the child output pipe");
    }
    if (SystemClock::instance().now().since(start) > request.readiness_bound) {
      return make_error(ErrorCode::timeout, "the target process did not report readiness in time",
                        captured_);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
#else
  static_cast<void>(request);
  return make_error(ErrorCode::unsupported, "the local-process adapter requires Windows");
#endif
}

bool ProcessSupervisor::running() {
#if defined(_WIN32)
  if (process_handle_ == nullptr) {
    return false;
  }
  if (exited_) {
    return false;
  }
  const DWORD wait = ::WaitForSingleObject(process_handle_, 0);
  if (wait == WAIT_OBJECT_0) {
    DWORD code = 0;
    ::GetExitCodeProcess(process_handle_, &code);
    exit_code_ = static_cast<int>(code);
    exited_ = true;
    return false;
  }
  return true;
#else
  return false;
#endif
}

Result<std::uint64_t> ProcessSupervisor::wait_exit(Duration bound) {
#if defined(_WIN32)
  if (process_handle_ == nullptr) {
    return make_error(ErrorCode::precondition_failed, "no child process is running");
  }
  const auto milliseconds = static_cast<DWORD>(
      std::max<std::int64_t>(1, bound.nanos() / 1000000));
  const DWORD wait = ::WaitForSingleObject(process_handle_, milliseconds);
  if (wait == WAIT_TIMEOUT) {
    return make_error(ErrorCode::timeout, "the target process did not exit in time");
  }
  DWORD code = 0;
  ::GetExitCodeProcess(process_handle_, &code);
  exit_code_ = static_cast<int>(code);
  exited_ = true;
  return static_cast<std::uint64_t>(code);
#else
  static_cast<void>(bound);
  return make_error(ErrorCode::unsupported, "the local-process adapter requires Windows");
#endif
}

Status ProcessSupervisor::stop(Duration grace) {
#if defined(_WIN32)
  if (process_handle_ == nullptr) {
    return ok_status();
  }
  if (!running()) {
    return ok_status();
  }
  // The caller is expected to have asked the control plane to shut down over the
  // transport; this is the bounded fallback.
  const auto milliseconds =
      static_cast<DWORD>(std::max<std::int64_t>(1, grace.nanos() / 1000000));
  const DWORD wait = ::WaitForSingleObject(process_handle_, milliseconds);
  if (wait == WAIT_TIMEOUT) {
    return kill();
  }
  DWORD code = 0;
  ::GetExitCodeProcess(process_handle_, &code);
  exit_code_ = static_cast<int>(code);
  exited_ = true;
  return ok_status();
#else
  static_cast<void>(grace);
  return make_error(ErrorCode::unsupported, "the local-process adapter requires Windows");
#endif
}

Status ProcessSupervisor::kill() {
#if defined(_WIN32)
  if (process_handle_ == nullptr) {
    return ok_status();
  }
  if (!running()) {
    return ok_status();
  }
  if (!::TerminateProcess(static_cast<HANDLE>(process_handle_), 137)) {
    return make_error(ErrorCode::io_error, "could not terminate the target process");
  }
  ::WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code);
  exit_code_ = static_cast<int>(code);
  exited_ = true;
  return ok_status();
#else
  return make_error(ErrorCode::unsupported, "the local-process adapter requires Windows");
#endif
}

}  // namespace fum::proc
