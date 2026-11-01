// tide: command-line driver.
//
//   tide check <file.tid>                 parse + semantic validation only
//   tide up <file.tid> [options]          full lifecycle; leaves the network running
//   tide down <file.tid> [--network N]    stop a network launched by `tide up`
#include <unistd.h>

#include <cstring>
#include <iostream>

#include "interp.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "solana_backend.hpp"

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

bool parse_args(int argc, char** argv, Args& a, std::string& err) {
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                err = s + " requires a value";
                return false;
            }
            out = argv[++i];
            return true;
        };
        if (s == "--network") { if (!value(a.network)) return false; }
        else if (s == "--scenario") {
            std::string v;
            if (!value(v)) return false;
            a.scenarios.push_back(v);
        } else if (s == "--state-dir") { if (!value(a.state_dir)) return false; }
        else if (s == "--validator") { if (!value(a.validator)) return false; }
        else if (s == "--solana") { if (!value(a.solana)) return false; }
        else if (s == "--no-scenarios") a.no_scenarios = true;
        else if (s == "--dry-run") a.dry_run = true;
        else if (s.starts_with("--")) {
            err = "unknown option " + s;
            return false;
        } else pos.push_back(s);
    }
    if (pos.size() != 2) {
        err = pos.empty() ? "missing command" : "expected a command and a file";
        return false;
    }
    a.command = pos[0];
    a.file = pos[1];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "help"))) {
        std::cout << usage;
        return 0;
    }
    if (argc >= 2 && (!std::strcmp(argv[1], "--version") || !std::strcmp(argv[1], "version"))) {
        std::cout << "tide 0.1.0\n";
        return 0;
    }
    Args args;
    std::string err;
    if (!parse_args(argc, argv, args, err)) {
        std::cerr << "tide: " << err << "\n\n" << usage;
        return 2;
    }
    if (args.command != "check" && args.command != "up" && args.command != "down") {
        std::cerr << "tide: unknown command '" << args.command << "'\n\n" << usage;
        return 2;
    }

    const bool color = ::isatty(STDERR_FILENO);
    tide::SourceManager sources;
    tide::Diagnostics diags(sources);
    auto finish = [&](int code) {
        diags.print(std::cerr, color);
        if (diags.error_count() || diags.warning_count())
            std::cerr << diags.error_count() << " error(s), " << diags.warning_count() << " warning(s)\n";
        return code;
    };

    // Parse -> AST -> semantic validation.
    std::error_code ec;
    std::filesystem::path path = std::filesystem::absolute(args.file, ec);
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

    // Select the network.
    const tide::ast::NetworkDecl* network = nullptr;
    auto networks = program->networks();
    if (!args.network.empty()) {
        network = program->find_network(args.network);
        if (!network) {
            std::cerr << "tide: no network named '" << args.network << "' in " << args.file << "\n";
            return finish(2);
        }
    } else if (networks.size() == 1) {
        network = networks.front();
    } else {
        std::cerr << "tide: " << (networks.empty() ? "the file declares no network" : "the file declares several networks; pick one with --network:");
        for (const auto* n : networks) std::cerr << " " << n->name.name;
        std::cerr << "\n";
        return finish(2);
    }

    std::filesystem::path state_root =
        args.state_dir.empty() ? tide::backend::default_state_root(path.parent_path()) : std::filesystem::path(args.state_dir);
    if (args.command == "down") {
        std::string error;
        if (!tide::backend::stop_network(state_root / network->name.name, std::cout, error)) {
            std::cerr << "tide: " << error << "\n";
            return finish(1);
        }
        std::cout << "network '" << network->name.name << "' stopped\n";
        return finish(0);
    }

    // Interpret configuration -> genesis -> bootstrap -> persist -> launch.
    tide::Interpreter interp(*program, diags, std::cout);
    if (!interp.init_globals()) return finish(1);
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
    tide::RunResult result = interp.run(*network, backend, ropts);
    if (!result.ok) return finish(1);
    if (result.launch) {
        std::cout << "\nnetwork '" << network->name.name << "' is running\n"
                  << "  rpc:   " << result.launch->rpc_url << "\n"
                  << "  state: " << result.launch->state_dir << "\n";
        if (result.launch->pid) std::cout << "  pid:   " << *result.launch->pid << "\n";
        std::cout << "  stop:  tide down " << args.file << (args.network.empty() ? "" : " --network " + args.network) << "\n";
    }
    if (result.scenarios_run)
        std::cout << "scenarios: " << result.scenarios_run - result.scenarios_failed << " passed, " << result.scenarios_failed
                  << " failed\n";
    return finish(result.scenarios_failed ? 3 : 0);
}
