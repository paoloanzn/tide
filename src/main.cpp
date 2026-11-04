// tide: command-line driver.
//
//   tide check <file.tid>                 parse + semantic validation only
//   tide up <file.tid> [options]          full lifecycle; leaves the network running
//   tide down <file.tid> [--network N]    stop a network launched by `tide up`
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "diag.hpp"
#include "interp.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "solana_backend.hpp"
#include "state_builder.hpp"

#ifndef TIDE_VERSION
#error "TIDE_VERSION is defined by CMakeLists.txt from project(VERSION)"
#endif

namespace {

constexpr const char* usage = R"(usage: tide <command> <file.tid> [options]

commands:
  check   parse and validate a script (no execution)
  up      run the lifecycle: configure, genesis, bootstrap, persist, launch, scenarios;
          the network keeps running after tide exits
  down    stop a network previously started with 'tide up'

options:
  --network <name>     network to use when the file declares several
  --scenario <name>    run only this scenario (repeatable)
  --no-scenarios       skip scenarios
  --dry-run            build and persist genesis state, print the launch command, start nothing
  --state-dir <dir>    where network state lives (default: <script dir>/.tide)
  --validator <path>   solana-test-validator binary (default: from PATH)
  --solana <path>      solana CLI binary used by 'deploy' (default: from PATH)
)";

struct Args {
    std::string command, file, network, state_dir, validator = "solana-test-validator", solana = "solana";
    std::vector<std::string> scenarios;
    bool no_scenarios = false, dry_run = false;
};

// Where the value of option `opt` is stored, or nullptr if `opt` takes no
// value. `--scenario` is repeatable, so each occurrence appends a new entry.
std::string* value_slot(Args& a, std::string_view opt) {
    if (opt == "--network") return &a.network;
    if (opt == "--scenario") return &a.scenarios.emplace_back();
    if (opt == "--state-dir") return &a.state_dir;
    if (opt == "--validator") return &a.validator;
    if (opt == "--solana") return &a.solana;
    return nullptr;
}

// `cli` is the full argument vector, program name included.
bool parse_args(const std::vector<std::string>& cli, Args& a, std::string& err) {
    std::vector<std::string> pos;
    for (std::size_t i = 1; i < cli.size(); ++i) {
        const std::string& s = cli[i];
        if (std::string* slot = value_slot(a, s)) {
            if (i + 1 >= cli.size()) {
                err = s + " requires a value";
                return false;
            }
            *slot = cli[++i];
        } else if (s == "--no-scenarios")
            a.no_scenarios = true;
        else if (s == "--dry-run")
            a.dry_run = true;
        else if (s.starts_with("--")) {
            err = "unknown option " + s;
            return false;
        } else
            pos.push_back(s);
    }
    if (pos.size() != 2) {
        err = pos.empty() ? "missing command" : "expected a command and a file";
        return false;
    }
    a.command = pos[0];
    a.file = pos[1];
    return true;
}

// Handles `help` / `version` and their aliases; returns true if one was asked for.
bool print_info(const std::vector<std::string>& cli) {
    if (cli.size() < 2) return false;
    const std::string& first = cli[1];
    if (first == "--help" || first == "-h" || first == "help") {
        std::cout << usage;
        return true;
    }
    if (first == "--version" || first == "version") {
        std::cout << "tide " << TIDE_VERSION << "\n";
        return true;
    }
    return false;
}

// The network named by --network, or the only one declared. Reports and
// returns nullptr when there is no such network or the choice is ambiguous.
const tide::ast::NetworkDecl* select_network(const tide::sema::Program& program, const Args& args) {
    if (!args.network.empty()) {
        const tide::ast::NetworkDecl* network = program.find_network(args.network);
        if (!network) std::cerr << "tide: no network named '" << args.network << "' in " << args.file << "\n";
        return network;
    }
    const auto networks = program.networks();
    if (networks.size() == 1) return networks.front();
    std::cerr << "tide: "
              << (networks.empty() ? "the file declares no network"
                                   : "the file declares several networks; pick one with --network:");
    for (const auto* n : networks) std::cerr << " " << n->name.name;
    std::cerr << "\n";
    return nullptr;
}

bool has_scenario(const tide::ast::NetworkDecl& network, std::string_view name) {
    return std::ranges::any_of(network.entries, [&](const tide::ast::NetworkEntry& e) {
        const auto* sc = std::get_if<tide::ast::ScenarioEntry>(&e.node);
        return sc != nullptr && sc->name.name == name;
    });
}

// Reports the first --scenario that `network` does not declare.
bool check_scenarios_exist(const tide::ast::NetworkDecl& network, const std::vector<std::string>& wanted) {
    for (const auto& name : wanted)
        if (!has_scenario(network, name)) {
            std::cerr << "tide: no scenario named '" << name << "' in network '" << network.name.name << "'\n";
            return false;
        }
    return true;
}

void print_launch_summary(const tide::backend::LaunchInfo& launch, const std::string& network, const Args& args) {
    std::cout << "\nnetwork '" << network << "' is running\n"
              << "  rpc:   " << launch.rpc_url << "\n"
              << "  state: " << launch.state_dir << "\n";
    if (launch.pid) std::cout << "  pid:   " << *launch.pid << "\n";
    std::cout << "  stop:  tide down " << args.file << (args.network.empty() ? "" : " --network " + args.network)
              << "\n";
}

// `tide up`: configuration -> genesis -> bootstrap -> persist -> launch, then
// scenarios. Returns the process exit code.
int run_up(const tide::sema::Program& program, const tide::ast::NetworkDecl& network, const Args& args,
           const std::filesystem::path& state_root, tide::Diagnostics& diags) {
    tide::Interpreter interp(program, diags, std::cout);
    if (!interp.init_globals()) return 1;
    tide::backend::SolanaBackendOptions bopts;
    bopts.state_root = state_root;
    bopts.validator_binary = args.validator;
    bopts.solana_cli = args.solana;
    bopts.dry_run = args.dry_run;
    tide::backend::SolanaTestValidatorBackend backend(bopts, diags, std::cout);
    tide::RunOptions ropts;
    ropts.dry_run = args.dry_run;
    ropts.run_scenarios = !args.no_scenarios;
    ropts.only_scenarios = args.scenarios;
    const tide::RunResult result = interp.run(network, backend, ropts);
    if (!result.ok) return 1;
    if (result.launch) print_launch_summary(*result.launch, network.name.name, args);
    if (result.scenarios_run != 0U)
        std::cout << "scenarios: " << result.scenarios_run - result.scenarios_failed << " passed, "
                  << result.scenarios_failed << " failed\n";
    return (result.scenarios_failed != 0U) ? 3 : 0;
}

// Diagnostics are colored only on a terminal, and like GCC and Clang never when TERM is unset or "dumb". A non-empty
// NO_COLOR (https://no-color.org) turns colors off too, for terminals that do support them, like Emacs' M-x compile.
bool use_color() {
    if (::isatty(STDERR_FILENO) == 0) return false;
    // NOLINTBEGIN(concurrency-mt-unsafe): no thread-safe getenv exists; Tide never modifies its environment
    const char* no_color = std::getenv("NO_COLOR");
    const char* term = std::getenv("TERM");
    // NOLINTEND(concurrency-mt-unsafe)
    if (no_color != nullptr && *no_color != '\0') return false;
    return term != nullptr && std::string_view(term) != "dumb";
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> raw_args(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string> cli(raw_args.begin(), raw_args.end());
    if (print_info(cli)) return 0;
    Args args;
    std::string err;
    if (!parse_args(cli, args, err)) {
        std::cerr << "tide: " << err << "\n\n" << usage;
        return 2;
    }
    if (args.command != "check" && args.command != "up" && args.command != "down") {
        std::cerr << "tide: unknown command '" << args.command << "'\n\n" << usage;
        return 2;
    }

    const bool color = use_color();
    tide::SourceManager sources;
    tide::Diagnostics diags(sources);
    auto finish = [&](int code) {
        diags.print(std::cerr, color);
        if ((diags.error_count() != 0U) || (diags.warning_count() != 0U))
            std::cerr << diags.error_count() << " error(s), " << diags.warning_count() << " warning(s)\n";
        return code;
    };

    // Parse -> AST -> semantic validation.
    std::error_code ec;
    const std::filesystem::path path = std::filesystem::absolute(args.file, ec);
    const tide::SourceFile* file = sources.load(path);
    if (!file) {
        std::cerr << "tide: cannot read '" << args.file << "'\n";
        return 1;
    }
    auto doc = tide::parse_document(*file, diags);
    if (!doc) return finish(1);
    auto program = tide::sema::analyze(*doc, sources, diags);
    if (!program) return finish(1);

    if (args.command == "check") {
        std::cout << args.file << ": ok (" << program->networks().size() << " network(s))\n";
        return finish(0);
    }

    const tide::ast::NetworkDecl* network = select_network(*program, args);
    if (!network || !check_scenarios_exist(*network, args.scenarios)) return finish(2);

    const std::filesystem::path state_root = args.state_dir.empty()
                                                 ? tide::backend::default_state_root(path.parent_path())
                                                 : std::filesystem::path(args.state_dir);
    if (args.command == "down") {
        std::string error;
        if (!tide::backend::stop_network(state_root / network->name.name, std::cout, error)) {
            std::cerr << "tide: " << error << "\n";
            return finish(1);
        }
        std::cout << "network '" << network->name.name << "' stopped\n";
        return finish(0);
    }

    return finish(run_up(*program, *network, args, state_root, diags));
}
