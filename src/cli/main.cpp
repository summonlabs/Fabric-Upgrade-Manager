// fum: command line interface for the Fabric Upgrade Manager runtime.
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "fum/engine/engine.hpp"

#include "commands.hpp"
#include "runtime_assembly.hpp"

namespace {

void print_usage(std::ostream& out) {
  out << "fum - Fabric Upgrade Manager (Summon Software Labs)\n"
      << "\n"
      << "usage: fum <command> [options]\n"
      << "\n"
      << "commands:\n"
      << "  plan          build and print an upgrade plan from a request document\n"
      << "  run           create, preflight, start and report in one process\n"
      << "  create        create a campaign from a request document\n"
      << "  preflight     resolve inventory, query compatibility and decide admission\n"
      << "  start         admit a validated campaign and execute it\n"
      << "  pause         pause a campaign at a stage boundary\n"
      << "  resume        resume a paused or blocked campaign (preflight must be current)\n"
      << "  abort         abort a campaign\n"
      << "  rollback      plan or execute a rollback\n"
      << "  status        show one campaign or every campaign\n"
      << "  explain       explain a campaign: state, evidence, policy, decisions\n"
      << "  inventory     inspect the resolved inventory\n"
      << "  provenance    inspect the provenance ledger\n"
      << "  decisions     inspect recorded decisions\n"
      << "  stats         show runtime and store statistics\n"
      << "\n"
      << "options:\n"
      << "  --state <dir>          durable state directory (default ./fum-state)\n"
      << "  --inventory <file>     target catalog (required)\n"
      << "  --registry <file>      compatibility registry export (required)\n"
      << "  --artifacts <file>     artifact payload catalog (required)\n"
      << "  --request <file>       plan request document\n"
      << "  --campaign <id>        campaign identity\n"
      << "  --control-plane <exe>  enable the real local-process adapter for this executable\n"
      << "  --reason <text>        reason recorded with pause/abort/start\n"
      << "  --allow-irreversible   acknowledge irreversible steps explicitly\n"
      << "  --parallel <n>         maximum concurrent activations\n"
      << "  --dry-run              admit without executing\n"
      << "  --plan-only            plan a rollback without executing it\n"
      << "  --executor <kind>      deterministic-inline | thread-pool\n"
      << "  --threads <n>          worker threads for the thread-pool executor\n"
      << "  --json                 machine readable output\n"
      << "  --verbose              debug logging on stderr\n"
      << "\n"
      << "exit codes: 0 success, 1 runtime failure, 2 usage, 3 refused by policy or\n"
      << "            precondition, 4 not found, 5 not integrated or unsupported\n";
}

struct ParsedArguments {
  fum::cli::CommandOptions command;
  fum::cli::AssemblyOptions assembly;
  std::string executor = "deterministic-inline";
  std::size_t threads = 4;
  bool help = false;
  bool valid = true;
  std::string error;
};

bool take_value(int argc, char** argv, int& index, std::string& target, std::string& error) {
  if (index + 1 >= argc) {
    error = std::string("missing value for ") + argv[index];
    return false;
  }
  target = argv[++index];
  return true;
}

ParsedArguments parse(int argc, char** argv) {
  ParsedArguments parsed;
  parsed.assembly.state_directory = "fum-state";
  if (argc < 2) {
    parsed.help = true;
    return parsed;
  }
  parsed.assembly.state_directory = "fum-state";
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    // Global options may appear before or after the command.
    if (!argument.empty() && argument[0] != '-') {
      if (parsed.command.command.empty()) {
        parsed.command.command = argument;
        if (parsed.command.command == "help") {
          parsed.help = true;
          return parsed;
        }
        continue;
      }
      parsed.error = "unexpected argument: " + argument;
      parsed.valid = false;
      return parsed;
    }
    if (argument == "--help" || argument == "-h") {
      parsed.help = true;
      return parsed;
    }
    if (argument == "--json") {
      continue;   // handled by main
    } else if (argument == "--verbose") {
      parsed.assembly.verbose = true;
    } else if (argument == "--allow-irreversible") {
      parsed.command.allow_irreversible = true;
    } else if (argument == "--dry-run") {
      parsed.command.dry_run = true;
    } else if (argument == "--plan-only") {
      parsed.command.plan_only = true;
    } else if (argument == "--state") {
      if (!take_value(argc, argv, i, parsed.assembly.state_directory, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--inventory") {
      if (!take_value(argc, argv, i, parsed.assembly.inventory_path, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--registry") {
      if (!take_value(argc, argv, i, parsed.assembly.registry_path, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--artifacts") {
      if (!take_value(argc, argv, i, parsed.assembly.artifacts_path, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--control-plane") {
      if (!take_value(argc, argv, i, parsed.assembly.control_plane_executable, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--request") {
      if (!take_value(argc, argv, i, parsed.command.request_path, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--campaign") {
      if (!take_value(argc, argv, i, parsed.command.campaign, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--reason") {
      if (!take_value(argc, argv, i, parsed.command.reason, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--executor") {
      if (!take_value(argc, argv, i, parsed.executor, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--threads") {
      std::string value;
      if (!take_value(argc, argv, i, value, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
      try {
        parsed.threads = static_cast<std::size_t>(std::stoull(value));
      } catch (const std::exception&) {
        parsed.error = "invalid --threads value";
        parsed.valid = false;
        return parsed;
      }
    } else if (argument == "--parallel") {
      std::string value;
      if (!take_value(argc, argv, i, value, parsed.error)) {
        parsed.valid = false;
        return parsed;
      }
      try {
        parsed.command.parallel = static_cast<std::uint32_t>(std::stoul(value));
      } catch (const std::exception&) {
        parsed.error = "invalid --parallel value";
        parsed.valid = false;
        return parsed;
      }
    } else {
      parsed.error = "unrecognized option: " + argument;
      parsed.valid = false;
      return parsed;
    }
  }
  if (parsed.command.command.empty()) {
    parsed.help = true;
  }
  return parsed;
}

}  // namespace

int main(int argc, char** argv) {
  const ParsedArguments parsed = parse(argc, argv);
  if (parsed.help) {
    print_usage(std::cout);
    return 0;
  }
  if (!parsed.valid) {
    std::cerr << "fum: " << parsed.error << "\n";
    print_usage(std::cerr);
    return 2;
  }

  bool json_output = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--json") {
      json_output = true;
    }
  }

  auto executor = fum::parse_executor_kind(parsed.executor);
  if (!executor.has_value()) {
    std::cerr << "fum: " << executor.error().to_string() << "\n";
    return 2;
  }
  fum::cli::AssemblyOptions assembly_options = parsed.assembly;
  assembly_options.executor = executor.value();
  assembly_options.worker_threads = parsed.threads;

  const bool needs_runtime = parsed.command.command != "help";
  if (!needs_runtime) {
    print_usage(std::cout);
    return 0;
  }

  auto assembly = fum::cli::assemble(assembly_options);
  if (!assembly.has_value()) {
    std::cerr << "fum: " << assembly.error().to_string() << "\n";
    return 5;
  }

  auto engine = fum::Engine::open(assembly.value().config, assembly.value().dependencies);
  if (!engine.has_value()) {
    std::cerr << "fum: " << engine.error().to_string() << "\n";
    return 1;
  }

  fum::cli::CommandContext context;
  context.engine = engine.value().get();
  context.out = &std::cout;
  context.err = &std::cerr;
  context.json = json_output;

  const int code = fum::cli::run_command(parsed.command, context);
  const fum::Status closed = engine.value()->shutdown();
  if (!closed.has_value() && code == 0) {
    std::cerr << "fum: shutdown failed: " << closed.error().to_string() << "\n";
    return 1;
  }
  return code;
}
