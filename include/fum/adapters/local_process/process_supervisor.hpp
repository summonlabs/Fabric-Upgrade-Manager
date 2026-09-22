// Real OS process supervision for the local-process upgrade path.
//
// The supervisor starts, stops and restarts an actual executable. Child stdout
// is read through a pipe; a bounded readiness wait uses the process exit as its
// only other escape, so a missing READY line is reported instead of hanging.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fum/core/result.hpp"
#include "fum/core/time.hpp"

namespace fum::proc {

struct [[nodiscard]] StartRequest {
  std::string executable;
  std::vector<std::string> arguments;
  std::string working_directory;
  std::string ready_prefix = "READY";
  Duration readiness_bound = Duration::from_seconds(20);
  Duration exit_bound = Duration::from_seconds(20);
};

struct [[nodiscard]] StartedProcess {
  std::uint64_t pid = 0;
  std::string ready_line;
  std::string trailing_output;
};

class [[nodiscard]] ProcessSupervisor {
 public:
  ProcessSupervisor() = default;
  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;
  ProcessSupervisor(ProcessSupervisor&& other) noexcept;
  ProcessSupervisor& operator=(ProcessSupervisor&& other) noexcept;
  ~ProcessSupervisor();

  [[nodiscard]] static Result<ProcessSupervisor> create();
  [[nodiscard]] Status start(const StartRequest& request);
  // Waits for the child's readiness line; returns an error when the child exits
  // first or when the readiness bound elapses.
  [[nodiscard]] Result<StartedProcess> await_ready(const StartRequest& request);
  [[nodiscard]] Status stop(Duration grace = Duration::from_seconds(5));
  [[nodiscard]] Status kill();
  [[nodiscard]] bool running();
  [[nodiscard]] Result<std::uint64_t> wait_exit(Duration bound);
  [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }
  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }
  [[nodiscard]] const std::string& captured_output() const noexcept { return captured_; }

 private:
  void close_handles();

  std::uint64_t pid_ = 0;
  void* process_handle_ = nullptr;
  void* thread_handle_ = nullptr;
  void* read_handle_ = nullptr;
  void* write_handle_ = nullptr;
  int exit_code_ = -1;
  bool exited_ = false;
  std::string captured_;
};

}  // namespace fum::proc
