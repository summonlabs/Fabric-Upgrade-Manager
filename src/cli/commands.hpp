#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "fum/engine/engine.hpp"

namespace fum::cli {

struct [[nodiscard]] CommandContext {
  Engine* engine = nullptr;
  std::ostream* out = nullptr;
  std::ostream* err = nullptr;
  bool json = false;
};

struct [[nodiscard]] CommandOptions {
  std::string command;
  std::string campaign;
  std::string request_path;
  std::string reason;
  bool allow_irreversible = false;
  bool dry_run = false;
  bool plan_only = false;
  std::uint32_t parallel = 0;
};

// Returns a process exit code.
[[nodiscard]] int run_command(const CommandOptions& options, const CommandContext& context);

}  // namespace fum::cli
