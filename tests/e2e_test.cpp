// Full lifecycle against a real solana-test-validator. Opt-in because it
// starts processes and takes ~20s:  TIDE_E2E=1 ./tide_tests "[e2e]"
#include <catch2/catch_test_macros.hpp>
#include <cpr/cpr.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "diag.hpp"
#include "interp.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "solana_backend.hpp"

namespace backend = tide::backend;
namespace fs = std::filesystem;
namespace sema = tide::sema;
using tide::Diagnostics;
using tide::Interpreter;
using tide::parse_document;
using tide::RunOptions;
using tide::RunResult;
using tide::SourceFile;
using tide::SourceManager;

namespace {

// clang-tidy's optional-access analysis cannot see that a preceding REQUIRE aborts the test, so values the test has
// already required are unwrapped through this explicit check (an empty optional fails the test with this message).
// Taking an lvalue keeps the returned reference from outliving a temporary optional.
template <class Optional>
decltype(auto) must(Optional& opt) {
    if (!opt) throw std::logic_error("expected a value, got an empty optional");
    return *opt;
}

}  // namespace

TEST_CASE("e2e: genesis, bootstrap, persist, detached launch, scenarios", "[e2e]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once at test start, nothing modifies the environment
    if (!std::getenv("TIDE_E2E")) SKIP("set TIDE_E2E=1 to run against solana-test-validator");

    SourceManager sm;
    Diagnostics diags(sm);
    const SourceFile* f = sm.load(fs::path(TIDE_EXAMPLES_DIR) / "07_e2e_tokens.tid");
    REQUIRE(f);
    auto doc = parse_document(*f, diags);
    REQUIRE(doc);
    auto prog = sema::analyze(must(doc), sm, diags);
    REQUIRE(prog);

    const fs::path state = fs::temp_directory_path() / "tide_e2e_state";
    fs::remove_all(state);
    std::ostringstream log;
    RunResult result;
    {
        Interpreter interp(*prog, diags, log);
        REQUIRE(interp.init_globals());
        backend::SolanaBackendOptions opts;
        opts.state_root = state;
        backend::SolanaTestValidatorBackend be(opts, diags, log);
        result = interp.run(*prog->networks().front(), be, {});
    }  // the backend is destroyed here: a detached network must survive it
    std::string report;
    for (const auto& d : diags.all()) report += diags.format(d);
    INFO(log.str() << report);
    REQUIRE(result.ok);
    CHECK(result.scenarios_run == 2);
    CHECK(result.scenarios_failed == 0);
    REQUIRE(result.launch);

    const cpr::Response health =
        cpr::Post(cpr::Url{must(result.launch).rpc_url}, cpr::Header{{"Content-Type", "application/json"}},
                  cpr::Body{R"({"jsonrpc":"2.0","id":1,"method":"getHealth"})"});
    CHECK(health.text.find("\"ok\"") != std::string::npos);

    std::string error;
    CHECK(backend::stop_network(state / "e2e", log, error));
    fs::remove_all(state);
}

TEST_CASE("backend dry run: one --bind-address, validator setting wins", "[backend]") {
    SourceManager sm;
    Diagnostics diags(sm);
    const SourceFile* f = sm.add("bind.tid", R"(
        network n {
          options { bind_address = "127.0.0.2"; }
          validator v { bind_address = "127.0.0.1"; rpc_port = 28899; }
          genesis { account seed("a").pubkey { lamports = 1 SOL; } }
        }
    )");
    auto doc = parse_document(*f, diags);
    REQUIRE(doc);
    auto prog = sema::analyze(must(doc), sm, diags);
    REQUIRE(prog);
    const fs::path state = fs::temp_directory_path() / "tide_bind_test";
    fs::remove_all(state);
    std::ostringstream log;
    Interpreter interp(*prog, diags, log);
    REQUIRE(interp.init_globals());
    backend::SolanaBackendOptions opts;
    opts.state_root = state;
    opts.dry_run = true;
    backend::SolanaTestValidatorBackend be(opts, diags, log);
    RunOptions ro;
    ro.dry_run = true;
    REQUIRE(interp.run(*prog->networks().front(), be, ro).ok);
    const std::string cmd = log.str().substr(log.str().find("launch command:"));
    CHECK(cmd.find("--bind-address 127.0.0.1") != std::string::npos);
    CHECK(cmd.find("127.0.0.2") == std::string::npos);
    CHECK(fs::exists(state / "n" / "manifest.json"));
    CHECK(fs::exists(state / "n" / "accounts"));
    fs::remove_all(state);
}

TEST_CASE("backend dry run refuses a running network's state directory", "[backend]") {
    SourceManager sm;
    Diagnostics diags(sm);
    const SourceFile* f = sm.add("running.tid", R"(
        network n { genesis { account seed("a").pubkey { lamports = 1 SOL; } } }
    )");
    auto doc = parse_document(*f, diags);
    REQUIRE(doc);
    auto prog = sema::analyze(must(doc), sm, diags);
    REQUIRE(prog);
    const fs::path state = fs::temp_directory_path() / "tide_dry_run_running_test";
    fs::remove_all(state);
    fs::create_directories(state / "n");
    // Any live process stands in for the running validator.
    std::ofstream(state / "n" / "validator.pid") << ::getpid();
    std::ostringstream log;
    Interpreter interp(*prog, diags, log);
    REQUIRE(interp.init_globals());
    backend::SolanaBackendOptions opts;
    opts.state_root = state;
    opts.dry_run = true;
    backend::SolanaTestValidatorBackend be(opts, diags, log);
    RunOptions ro;
    ro.dry_run = true;
    CHECK_FALSE(interp.run(*prog->networks().front(), be, ro).ok);
    CHECK(diags.error_count() == 1);
    CHECK_FALSE(fs::exists(state / "n" / "manifest.json"));
    CHECK_FALSE(fs::exists(state / "n" / "accounts"));
    fs::remove_all(state);
}
