// Full lifecycle against a real solana-test-validator. Opt-in because it
// starts processes and takes ~20s:  TIDE_E2E=1 ./tide_tests "[e2e]"
#include <catch2/catch_test_macros.hpp>

#include <cpr/cpr.h>

#include <cstdlib>
#include <sstream>

#include "interp.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "solana_backend.hpp"

using namespace tide;
namespace fs = std::filesystem;

TEST_CASE("e2e: genesis, bootstrap, persist, detached launch, scenarios", "[e2e]") {
    if (!std::getenv("TIDE_E2E")) SKIP("set TIDE_E2E=1 to run against solana-test-validator");

    SourceManager sm;
    Diagnostics diags(sm);
    const SourceFile* f = sm.load(fs::path(TIDE_EXAMPLES_DIR) / "07_e2e_tokens.tid");
    REQUIRE(f);
    auto doc = parse_document(*f, diags);
    REQUIRE(doc);
    auto prog = sema::analyze(*doc, sm, diags);
    REQUIRE(prog);

    fs::path state = fs::temp_directory_path() / "tide_e2e_state";
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

    cpr::Response health = cpr::Post(cpr::Url{result.launch->rpc_url}, cpr::Header{{"Content-Type", "application/json"}},
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
    auto prog = sema::analyze(*doc, sm, diags);
    REQUIRE(prog);
    fs::path state = fs::temp_directory_path() / "tide_bind_test";
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
    std::string cmd = log.str().substr(log.str().find("launch command:"));
    CHECK(cmd.find("--bind-address 127.0.0.1") != std::string::npos);
    CHECK(cmd.find("127.0.0.2") == std::string::npos);
    CHECK(fs::exists(state / "n" / "manifest.json"));
    CHECK(fs::exists(state / "n" / "accounts"));
    fs::remove_all(state);
}
