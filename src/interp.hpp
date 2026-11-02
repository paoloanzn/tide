// Direct AST interpreter. Executes a checked program (see sema.hpp) and drives
// a network backend through the lifecycle defined in state_builder.hpp.
#pragma once

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sema.hpp"
#include "state_builder.hpp"

namespace tide {

struct RunOptions {
    bool run_scenarios = true;
    std::vector<std::string> only_scenarios;  // empty: all scenarios
    // Configure, build genesis and persist only: no bootstrap, launch or scenarios.
    bool dry_run = false;
};

struct RunResult {
    bool ok = false;  // lifecycle reached the end (network launched, or dry run persisted)
    std::size_t scenarios_run = 0;
    std::size_t scenarios_failed = 0;
    std::optional<backend::LaunchInfo> launch;
};

class Interpreter {
public:
    // `out` receives progress messages and `print(...)` output.
    Interpreter(const sema::Program& program, Diagnostics& diags, std::ostream& out);
    ~Interpreter();
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;
    Interpreter(Interpreter&&) = delete;
    Interpreter& operator=(Interpreter&&) = delete;

    // Evaluates module-level bindings of every module, imports first.
    bool init_globals();

    // Runs the lifecycle of `network`: configuration, genesis, bootstrap,
    // persist, launch, scenarios, detach. Requires init_globals() first.
    RunResult run(const ast::NetworkDecl& network, backend::NetworkBackend& backend, const RunOptions& options);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tide
