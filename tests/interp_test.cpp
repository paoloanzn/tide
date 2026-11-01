// Interpreter tests against an in-memory backend: lifecycle ordering, genesis
// construction and validation, runtime requests, and language semantics.
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <functional>
#include <sstream>

#include "interp.hpp"
#include "parser.hpp"
#include "sema.hpp"

using namespace tide;
using namespace tide::backend;
namespace fs = std::filesystem;

namespace {

// Records every call crossing the backend boundary.
struct FakeBackend final : NetworkBackend, GenesisBuilder, RuntimeClient {
    NetworkConfig config;
    std::map<Pubkey, AccountView> accounts;
    std::vector<ProgramSpec> programs;
    std::vector<CloneSpec> clones;
    std::vector<FeatureSpec> features;
    std::vector<SysvarSpec> sysvars;
    std::vector<TransactionRequest> txs;
    std::vector<std::string> events;
    std::function<TxResult(const TransactionRequest&)> on_tx = [](const TransactionRequest&) {
        return TxResult{.success = true, .signature = "sig", .logs = {"Program log: ok"}, .slot = 7};
    };
    std::uint64_t current_slot = 0;

    void configure(const NetworkConfig& c) override {
        config = c;
        events.push_back("configure");
    }
    GenesisBuilder& genesis() override { return *this; }
    RuntimeClient& start_bootstrap() override {
        events.push_back("start_bootstrap");
        return *this;
    }
    void persist() override { events.push_back("persist"); }
    RuntimeClient& launch() override {
        events.push_back("launch");
        return *this;
    }
    LaunchInfo detach() override {
        events.push_back("detach");
        return {"http://fake", "/fake", 1};
    }
    void abort() noexcept override { events.push_back("abort"); }

    void put_account(const AccountSpec& s) override {
        accounts[s.address] = {s.address, s.lamports, s.owner, s.data, s.executable, s.rent_epoch};
    }
    void put_program(const ProgramSpec& s) override { programs.push_back(s); }
    void clone_account(const CloneSpec& s) override {
        clones.push_back(s);
        accounts[s.address] = {s.address, s.overrides.lamports.value_or(1), sol::ids::system_program, {}, false, 0};
    }
    void remove_account(const DeleteSpec& s) override { accounts.erase(s.address); }
    void set_feature(const FeatureSpec& s) override { features.push_back(s); }
    void set_sysvar(const SysvarSpec& s) override { sysvars.push_back(s); }
    void fork(const ForkSpec&) override { events.push_back("fork"); }
    std::optional<AccountView> account(const Pubkey& a) const override {
        auto it = accounts.find(a);
        if (it == accounts.end()) return std::nullopt;
        return it->second;
    }

    TxResult send_transaction(const TransactionRequest& r) override {
        txs.push_back(r);
        events.push_back("tx");
        return on_tx(r);
    }
    TxResult airdrop(const Pubkey& to, std::uint64_t lamports, Loc) override {
        events.push_back("airdrop");
        auto& a = accounts[to];
        a.address = to;
        a.lamports += lamports;
        return TxResult{.success = true, .signature = "airdrop-sig", .slot = 1};
    }
    TxResult deploy(const DeployRequest&) override { return TxResult{.success = true}; }
    std::optional<AccountView> account(const Pubkey& a) override { return std::as_const(*this).account(a); }
    std::uint64_t slot() override { return current_slot++; }
    std::uint64_t epoch() override { return 0; }
};

struct Run {
    SourceManager sm;
    Diagnostics diags{sm};
    std::optional<ast::Document> doc;
    std::unique_ptr<sema::Program> prog;
    std::ostringstream out;
    FakeBackend backend;
    RunResult result;

    bool has(std::string_view text, Severity sev = Severity::error) const {
        for (const auto& d : diags.all())
            if (d.severity == sev && d.message.find(text) != std::string::npos) return true;
        return false;
    }
    std::string dump() const {
        std::string s;
        for (const auto& d : diags.all()) s += diags.format(d);
        return s + out.str();
    }
};

std::unique_ptr<Run> run(const std::string& src, RunOptions opts = {}, fs::path path = "test.tid",
                         std::function<void(FakeBackend&)> setup = {}) {
    auto r = std::make_unique<Run>();
    const SourceFile* f = r->sm.add(std::move(path), src);
    r->doc = parse_document(*f, r->diags);
    REQUIRE(r->doc);
    r->prog = sema::analyze(*r->doc, r->sm, r->diags);
    INFO(r->dump());
    REQUIRE(r->prog);
    if (setup) setup(r->backend);
    Interpreter interp(*r->prog, r->diags, r->out);
    if (!interp.init_globals()) return r;
    auto nets = r->prog->networks();
    REQUIRE(nets.size() == 1);
    r->result = interp.run(*nets[0], r->backend, opts);
    return r;
}

// Runs `body` inside genesis and returns what print() wrote.
std::string eval_print(const std::string& decls, const std::string& body) {
    auto r = run(decls + "\nnetwork n { genesis {\n" + body + "\n} }");
    INFO(r->dump());
    CHECK(r->result.ok);
    std::string text = r->out.str();
    std::size_t begin = text.find("==> building genesis state\n") + 27;
    std::size_t end = text.rfind('\n', text.find("genesis account(s) defined"));
    return text.substr(begin, end + 1 - begin);
}

Pubkey pk(const char* name) { return sol::Keypair::from_name(name).pubkey; }

fs::path temp_dir(const char* name) {
    fs::path d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

}  // namespace

// ------------------------------------------------------------ lifecycle --

TEST_CASE("lifecycle runs in order and detaches", "[interp]") {
    auto r = run(R"(
        keypair a = seed("a");
        network n {
          options { slots_per_epoch = 32; startup_timeout = 2 s; }
          validator v { rpc_port = 9000; }
          genesis { account a.pubkey { lamports = 1 SOL; } }
          bootstrap { airdrop a.pubkey amount 2 SOL; }
          scenario s1 { assert balance(a.pubkey) == 3 SOL; }
          scenario s2 { assert false, "always fails"; }
        }
    )");
    INFO(r->dump());
    REQUIRE(r->result.ok);
    CHECK(r->backend.events ==
          std::vector<std::string>{"configure", "start_bootstrap", "airdrop", "persist", "launch", "detach"});
    CHECK(r->backend.config.slots_per_epoch == 32u);
    CHECK(r->backend.config.startup_timeout_ms == 2000);
    REQUIRE(r->backend.config.validators.size() == 1);
    CHECK(r->backend.config.validators[0].rpc_port == 9000);
    CHECK(r->result.scenarios_run == 2);
    CHECK(r->result.scenarios_failed == 1);
    CHECK(r->has("scenario 's2' failed: assertion failed: always fails"));
}

TEST_CASE("dry run stops after persisting genesis", "[interp]") {
    RunOptions opts;
    opts.dry_run = true;
    auto r = run("network n { genesis { account seed(\"x\").pubkey { lamports = 5; } } bootstrap { airdrop seed(\"x\").pubkey amount 1; } }", opts);
    REQUIRE(r->result.ok);
    CHECK(r->backend.events == std::vector<std::string>{"configure", "persist"});
    CHECK(r->backend.accounts.size() == 1);
}

TEST_CASE("scenario filter and skipping", "[interp]") {
    RunOptions opts;
    opts.only_scenarios = {"b"};
    auto r = run("network n { scenario a { assert false; } scenario b { print(\"in b\"); } }", opts);
    CHECK(r->result.scenarios_run == 1);
    CHECK(r->result.scenarios_failed == 0);
    CHECK(r->out.str().find("in b") != std::string::npos);
    RunOptions none;
    none.run_scenarios = false;
    CHECK(run("network n { scenario a { assert false; } }", none)->result.scenarios_run == 0);
}

TEST_CASE("bootstrap failures abort the lifecycle", "[interp]") {
    auto r = run(R"(
        keypair a = seed("a");
        network n {
          bootstrap {
            transaction { signers = [a]; instruction system_program() { accounts = [a.pubkey]; data = system_transfer(1); } }
          }
        }
    )", {}, "test.tid", [](FakeBackend& b) {
        b.on_tx = [](const TransactionRequest&) { return TxResult{.signature = "s", .error = "InstructionError", .logs = {"Program log: boom"}}; };
    });
    CHECK_FALSE(r->result.ok);
    CHECK(r->has("transaction failed: InstructionError"));
    CHECK(r->has("Program log: boom"));
    CHECK(r->backend.events.back() == "abort");
}

// --------------------------------------------------------------- genesis --

TEST_CASE("genesis builds SPL state from the design example", "[interp]") {
    std::ifstream in(fs::path(TIDE_EXAMPLES_DIR) / "03_spl_mint.tid");
    std::stringstream ss;
    ss << in.rdbuf();
    auto r = run(ss.str());
    INFO(r->dump());
    REQUIRE(r->result.ok);
    CHECK_FALSE(r->diags.has_errors());
    CHECK(r->diags.warning_count() == 0);  // supply matches the declared balances
    const auto& mint = r->backend.accounts.at(pk("asset"));
    CHECK(mint.owner == sol::ids::token_program);
    CHECK(mint.lamports == sol::rent_exempt_minimum(82));
    auto m = sol::decode_mint(mint.data);
    REQUIRE(m);
    CHECK(m->decimals == 6);
    CHECK(m->supply == 125'000'000);
    CHECK(m->mint_authority == pk("issuer"));
    CHECK_FALSE(m->freeze_authority);
    Pubkey ata = sol::associated_token_address(pk("holder"), pk("asset"), sol::ids::token_program);
    auto t = sol::decode_token_account(r->backend.accounts.at(ata).data);
    REQUIRE(t);
    CHECK(t->owner == pk("holder"));
    CHECK(t->amount == 125'000'000);
}

TEST_CASE("genesis cross-declaration validation", "[interp]") {
    std::string pre = "keypair a = seed(\"a\");\nkeypair m = seed(\"m\");\n";
    auto dup = run(pre + "network n { genesis {\n account a.pubkey { lamports = 1 SOL; }\n account a.pubkey { lamports = 2 SOL; }\n} }");
    CHECK_FALSE(dup->result.ok);
    REQUIRE(dup->has("is already defined in genesis (as account)"));
    CHECK(dup->diags.all().front().notes.size() == 1);

    auto wrong_ata = run(pre + "network n { genesis { associated_token_account a.pubkey { mint = m.pubkey; owner = a.pubkey; } } }");
    CHECK(wrong_ata->has("is not the associated token account of owner"));

    auto supply = run(pre + "network n { genesis { mint m.pubkey { decimals = 0; supply = 10; }\n"
                            "  associated_token_account ata(a.pubkey, m.pubkey) { mint = m.pubkey; owner = a.pubkey; amount = 3; } } }");
    CHECK(supply->result.ok);
    CHECK(supply->has("has supply 10 but its token accounts in genesis hold 3", Severity::warning));

    auto program_mismatch = run(pre + "network n { genesis { mint m.pubkey { decimals = 0; token_program = token_2022_program(); }\n"
                                      "  token_account a.pubkey { mint = m.pubkey; owner = a.pubkey; } } }");
    CHECK(program_mismatch->has("uses a different token program than its mint"));

    auto ext = run(pre + "network n { genesis { mint m.pubkey { decimals = 0; extension non_transferable {} } } }");
    CHECK(ext->has("requires token_program = token_2022_program()"));

    auto delegated = run(pre + "network n { genesis { token_account a.pubkey { mint = m.pubkey; owner = a.pubkey; amount = 1; "
                               "delegate = m.pubkey; delegated_amount = 2; } } }");
    CHECK(delegated->has("exceeds the account amount"));

    auto missing_bin = run(pre + "network n { genesis { program p { id = m.pubkey; binary = \"/nonexistent/p.so\"; } } }");
    CHECK(missing_bin->has("does not exist"));

    auto rent = run(pre + "network n { genesis { account a.pubkey { lamports = 10; data = hex(\"00\"); } } }");
    CHECK(rent->has("below the rent-exempt minimum", Severity::warning));

    auto del = run(pre + "network n { genesis { account a.pubkey { lamports = 1 SOL; } delete account a.pubkey; delete account m.pubkey; } }");
    CHECK(del->has("cannot delete"));
    CHECK(del->backend.accounts.empty());
}

TEST_CASE("Token-2022 mints carry TLV extensions", "[interp]") {
    auto r = run(R"(
        keypair m = seed("m");
        network n { genesis {
          mint m.pubkey {
            token_program = token_2022_program(); decimals = 2;
            extension mint_close_authority { close_authority = m.pubkey; }
            extension non_transferable {}
          }
        } }
    )");
    REQUIRE(r->result.ok);
    const auto& data = r->backend.accounts.at(pk("m")).data;
    CHECK(data.size() == 165 + 1 + (4 + 32) + 4);
    CHECK(data[165] == 1);
    Pubkey m = pk("m");
    CHECK(std::equal(m.bytes.begin(), m.bytes.end(), data.begin() + 170));
}

TEST_CASE("lookup tables, sysvars, features and clones reach the backend", "[interp]") {
    auto r = run(R"(
        keypair a = seed("a");
        network n { genesis {
          lookup_table a.pubkey { authority = a.pubkey; addresses = [system_program(), token_program()]; }
          sysvar clock { slot = 100; }
          feature pubkey("11111111111111111111111111111112") = disabled;
          clone account seed("c").pubkey from rpc("devnet") at slot 5 { lamports = 9; };
        } }
    )");
    INFO(r->dump());
    REQUIRE(r->result.ok);
    const auto& lut = r->backend.accounts.at(pk("a"));
    CHECK(lut.owner == sol::ids::address_lookup_table_program);
    CHECK(lut.data.size() == 56 + 64);
    REQUIRE(r->backend.sysvars.size() == 1);
    CHECK(std::get<__int128>(r->backend.sysvars[0].fields.at("slot")) == 100);
    REQUIRE(r->backend.features.size() == 1);
    CHECK_FALSE(r->backend.features[0].enabled);
    REQUIRE(r->backend.clones.size() == 1);
    CHECK(r->backend.clones[0].url == "devnet");
    CHECK(r->backend.clones[0].slot == 5u);
    CHECK(r->backend.clones[0].overrides.lamports == 9u);
}

// Uses every key the checker accepts in every record-like block. Any key that
// passes sema without an interpreter handler fails here as an internal error
// instead of being silently dropped.
TEST_CASE("every checker-accepted field has an interpreter handler", "[interp]") {
    fs::path dir = temp_dir("tide_all_fields_test");
    std::ofstream(dir / "p.so") << "elf";
    std::ofstream(dir / "p.json") << R"({"instructions": [{"name": "go", "accounts": [], "args": []}]})";
    std::string src = R"(
        keypair a = seed("a");
        keypair p = seed("p");
        let m = seed("mint").pubkey;
        network n {
          options {
            slots_per_epoch = 64; ticks_per_slot = 8; ledger = "led"; faucet_sol = 1.5; compute_unit_limit = 1000;
            limit_ledger_size = 10000; warp_slot = 3; bind_address = "127.0.0.1"; startup_timeout = 5s; confirm_timeout = 2s;
          }
          validator v {
            rpc_port = 1111; gossip_port = 2222; faucet_port = 3333; dynamic_port_range = "4000-4100";
            identity = a; bind_address = "127.0.0.1";
          }
          genesis {
            account seed("acc").pubkey { lamports = 1 SOL; owner = system_program(); data = [1, 2]; executable = false; rent_epoch = 0; }
            program p { id = p.pubkey; binary = "p.so"; loader = bpf_upgradeable_loader(); upgrade_authority = a.pubkey; idl = "p.json"; }
            mint m { token_program = token_program(); decimals = 6; mint_authority = a.pubkey; freeze_authority = null;
                     supply = 10; lamports = 1 SOL; }
            token_account seed("ta").pubkey { token_program = token_program(); mint = m; owner = a.pubkey; amount = 4;
                     delegate = a.pubkey; delegated_amount = 1; state = "initialized"; close_authority = null; lamports = 1 SOL; }
            associated_token_account ata(a.pubkey, m) { token_program = token_program(); mint = m; owner = a.pubkey;
                     amount = 6; lamports = 1 SOL; }
            lookup_table seed("lut").pubkey { authority = a.pubkey; addresses = [m]; deactivation_slot = 9; last_extended_slot = 1;
                     lamports = 1 SOL; }
            clone account seed("c").pubkey from rpc("devnet") { lamports = 1; owner = system_program(); data = [0];
                     executable = false; rent_epoch = 0; };
          }
          bootstrap {
            transaction { fee_payer = a.pubkey; signers = [a]; compute_unit_limit = 5000; compute_unit_price = 1;
              instruction memo_program() { accounts = [a.pubkey]; data = memo("hi"); } }
            invoke p.pubkey with { method = "go"; accounts = {}; args = {}; signers = [a]; fee_payer = a.pubkey;
              idl = "p.json"; compute_unit_limit = 5000; remaining_accounts = [readonly(m)]; };
          }
        }
    )";
    auto r = run(src, {}, dir / "main.tid");
    INFO(r->dump());
    REQUIRE(r->result.ok);
    const NetworkConfig& c = r->backend.config;
    CHECK(c.slots_per_epoch == 64u);
    CHECK(c.confirm_timeout_ms == 2000);
    CHECK(c.faucet_sol == 1.5);
    REQUIRE(c.validators.size() == 1);
    CHECK(c.validators[0].dynamic_port_range == "4000-4100");
    CHECK(c.validators[0].identity.has_value());
    REQUIRE(r->backend.txs.size() == 2);
    CHECK(r->backend.txs[0].compute_unit_price == 1u);
    CHECK(r->backend.txs[1].instructions[0].accounts.size() == 1);  // remaining_accounts
    CHECK(r->backend.clones.at(0).overrides.executable == false);
    fs::remove_all(dir);
}

// --------------------------------------------------------------- runtime --

TEST_CASE("transactions become backend requests with inferred account metas", "[interp]") {
    auto r = run(R"(
        keypair a = seed("a");
        keypair b = seed("b");
        network n {
          bootstrap {
            transaction as tx {
              signers = [a, b];
              compute_unit_limit = 200000;
              instruction token_program() { accounts = [b.pubkey, readonly(system_program()), a.pubkey]; data = spl_transfer(5); }
            }
            assert tx.success && tx.slot == 7 && tx.logs[0] == "Program log: ok";
          }
        }
    )");
    INFO(r->dump());
    REQUIRE(r->result.ok);
    REQUIRE(r->backend.txs.size() == 1);
    const auto& req = r->backend.txs[0];
    CHECK(req.fee_payer == pk("a"));
    CHECK(req.compute_unit_limit == 200000u);
    REQUIRE(req.instructions.size() == 1);
    const auto& ix = req.instructions[0];
    CHECK(ix.data == sol::spl_transfer_data(5));
    REQUIRE(ix.accounts.size() == 3);
    CHECK((ix.accounts[0].is_signer && ix.accounts[0].is_writable));   // b: signer by membership
    CHECK((!ix.accounts[1].is_signer && !ix.accounts[1].is_writable)); // readonly()
    CHECK(ix.accounts[2].is_signer);                                    // a: fee payer
}

TEST_CASE("fee payer must sign", "[interp]") {
    auto r = run(R"(
        keypair a = seed("a");
        network n { bootstrap { transaction { fee_payer = seed("other").pubkey; signers = [a];
          instruction memo_program() { data = memo("x"); } } } }
    )");
    CHECK(r->has("has no matching keypair in 'signers'"));
}

TEST_CASE("invoke encodes Anchor instructions from the program IDL", "[interp]") {
    fs::path dir = temp_dir("tide_invoke_test");
    std::ofstream(dir / "counter.so") << "not really elf";
    std::ofstream(dir / "counter.json") << R"({
      "address": "11111111111111111111111111111111",
      "instructions": [{
        "name": "increment",
        "discriminator": [11, 18, 104, 9, 104, 174, 59, 33],
        "accounts": [{"name": "state", "writable": true}, {"name": "authority", "signer": true},
                     {"name": "system_program", "address": "11111111111111111111111111111111"}],
        "args": [{"name": "by", "type": "u64"}, {"name": "label", "type": {"option": "string"}}]
      }],
      "accounts": [{"name": "Counter", "discriminator": [255, 176, 4, 245, 188, 253, 124, 25]}],
      "types": [{"name": "Counter", "type": {"kind": "struct", "fields": [{"name": "value", "type": "u64"}]}}]
    })";
    std::ofstream(dir / "legacy.json") << R"({
      "instructions": [{"name": "setValue", "accounts": [{"name": "counterState", "isMut": true, "isSigner": false}],
                        "args": [{"name": "newValue", "type": "u32"}]}],
      "accounts": [{"name": "Legacy", "type": {"kind": "struct", "fields": [{"name": "flag", "type": "bool"}]}}]
    })";
    std::string src = R"(
        keypair admin = seed("admin");
        keypair counter = seed("counter");
        let state = pda(counter.pubkey, ["state"]);
        network n {
          genesis {
            program counter { id = counter.pubkey; binary = "counter.so"; upgrade_authority = admin.pubkey; idl = "counter.json"; }
            account state { lamports = rent_exempt(16); owner = counter.pubkey; data = anchor("Counter", { value: 7 }); }
          }
          bootstrap {
            invoke counter.pubkey with { method = "increment"; accounts = { state: state, authority: admin.pubkey }; args = { by: 2 }; signers = [admin]; };
            invoke counter.pubkey with { method = "set_value"; idl = "legacy.json"; accounts = { counter_state: state }; args = { new_value: 9 }; signers = [admin]; };
          }
        }
    )";
    auto r = run(src, {}, dir / "main.tid");
    INFO(r->dump());
    REQUIRE(r->result.ok);
    REQUIRE(r->backend.programs.size() == 1);
    CHECK(r->backend.programs[0].upgrade_authority == pk("admin"));
    // anchor("Counter", ...) uses the IDL's discriminator and layout.
    Pubkey state = sol::find_program_address({sol::Bytes{'s', 't', 'a', 't', 'e'}}, pk("counter")).first;
    CHECK(sol::hex_encode(r->backend.accounts.at(state).data) == "ffb004f5bcfd7c190700000000000000");

    REQUIRE(r->backend.txs.size() == 2);
    const auto& ix = r->backend.txs[0].instructions[0];
    CHECK(ix.program_id == pk("counter"));
    CHECK(sol::hex_encode(ix.data) == "0b12680968ae3b21" "0200000000000000" "00");
    REQUIRE(ix.accounts.size() == 3);
    CHECK(ix.accounts[0].pubkey == state);
    CHECK(ix.accounts[0].is_writable);
    CHECK(ix.accounts[1].pubkey == pk("admin"));
    CHECK(ix.accounts[1].is_signer);
    CHECK(ix.accounts[2].pubkey == sol::ids::system_program);
    // Legacy IDL: derived discriminator, camelCase names matched from snake_case.
    const auto& legacy = r->backend.txs[1].instructions[0];
    auto disc = sol::anchor_discriminator("global", "set_value");
    CHECK(std::equal(disc.begin(), disc.end(), legacy.data.begin()));
    CHECK(sol::hex_encode(sol::ByteView(legacy.data).subspan(8)) == "09000000");

    auto bad = run(R"(
        keypair counter = seed("counter");
        network n {
          genesis { program counter { id = counter.pubkey; binary = "counter.so"; idl = "counter.json"; } }
          bootstrap { invoke counter.pubkey with { method = "increment"; accounts = { state: counter.pubkey }; args = { by: 1, extra: 2 }; signers = [counter]; }; }
        }
    )", {}, dir / "bad.tid");
    CHECK(bad->has("instruction 'increment' has no argument 'extra'"));
    fs::remove_all(dir);
}

TEST_CASE("await polls state queries", "[interp]") {
    auto r = run("network n { scenario s { await slot() >= 3; print(\"done\"); } }");
    INFO(r->dump());
    CHECK(r->result.scenarios_failed == 0);
    CHECK(r->backend.current_slot >= 4);
}

// -------------------------------------------------------------- language --

TEST_CASE("functions, recursion and control flow", "[interp]") {
    std::string decls = R"(
        fn fib(n: int) -> int { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }
        fn sum_odd(limit: int) -> int {
          let total = 0;
          let i = 0;
          while true {
            i += 1;
            if i > limit { break; }
            if i % 2 == 0 { continue; }
            total += i;
          }
          return total;
        }
    )";
    CHECK(eval_print(decls, "print(fib(15), sum_odd(10));\nfor x in [1, 2] { for y in 0..=1 { print(x * 10 + y); } }") ==
          "610 25\n10\n11\n20\n21\n");
}

TEST_CASE("values have copy semantics", "[interp]") {
    CHECK(eval_print("", "let a = {xs: [1, 2]};\nlet b = a;\nb.xs[0] = 9;\nprint(a.xs[0], b.xs[0]);").starts_with("1 9\n"));
}

TEST_CASE("expressions evaluate with exact arithmetic", "[interp]") {
    CHECK(eval_print("", "print(7 / 2, -7 % 3, 1.5 * 2, 1 SOL + 5 lamports, 2 s + 250 ms, \"a\" + \"b\", hex(\"01\") + u8(2));")
              .starts_with("3 -1 3 1000000005 2250ms ab 0x0102\n"));
    CHECK(eval_print("", "print(len([1,2,3]), len(\"abc\"), str(true), 3 > 2.5, [1, 2] == [1, 2], null == null);")
              .starts_with("3 3 true true true true\n"));
    CHECK(eval_print("keypair a = seed(\"alice\");", "print(a.pubkey == seed(\"alice\").pubkey, sol(2), rent_exempt(0));")
              .starts_with("true 2000000000 890880\n"));
}

TEST_CASE("runtime errors are positioned", "[interp]") {
    auto net = [](const std::string& body) { return "network n { genesis {\n" + body + "\n} }"; };
    auto div = run(net("let z = 0;\nlet x = 1 / z;"));
    CHECK(div->has("division by zero"));
    CHECK(div->sm.locate(div->diags.all().front().loc.ptr).line == 3);
    CHECK(run(net("let big = 170141183460469231731687303715884105727;\nlet x = big + 1;"))->has("integer overflow"));
    CHECK(run(net("let xs = [1];\nlet y = xs[3];"))->has("index 3 is out of bounds for a list of length 1"));
    CHECK(run(net("let xs = [1];\nxs[-1] = 2;"))->has("index -1 is out of bounds for a list of length 1"));
    CHECK(run(net("let b = hex(\"0102\");\nlet y = b[2];"))->has("index 2 is out of bounds for bytes of length 2"));
    CHECK(run(net("let b = hex(\"0102\");\nb[5] += 1;"))->has("index 5 is out of bounds for bytes of length 2"));
    CHECK(run(net("let a = 200;\nlet b: u8 = a + 100;"))->has("300 is out of range for u8"));
    CHECK(run(net("assert 1 == 2, \"math\";"))->has("assertion failed: math"));
    CHECK(run("fn loop_forever(n: int) -> int { return loop_forever(n + 1); }\nnetwork n { genesis { let x = loop_forever(0); } }")
              ->has("maximum call depth"));
    auto acct = run(net("let x = account_info(seed(\"nobody\").pubkey).lamports;"));
    CHECK(acct->has("cannot read member 'lamports' of null"));
}

TEST_CASE("borsh schemas encode and decode", "[interp]") {
    std::string decls = R"(
        schema Inner using borsh { flag: bool; }
        schema Rec using borsh { a: u16; b: i8; s: string; xs: [u8]; fixed: [u8; 2]; o: Option<u32>; t: (u8, string); inner: Inner; f: f32; }
        schema Acc using anchor { v: u64; }
    )";
    std::string out = eval_print(decls, R"(
        let bytes = borsh(Rec, { a: 258, b: -1, s: "hi", xs: hex("0a0b"), fixed: [1, 2], t: [3, "z"], inner: { flag: true }, f: 1.5 });
        print(bytes);
        let back = decode(Rec, bytes);
        print(back.a, back.b, back.s, back.xs, back.fixed, back.o, back.t, back.inner.flag, back.f);
        let acc = anchor(Acc, { v: 5 });
        print(len(acc), decode(Acc, acc).v);
    )");
    CHECK(out.starts_with("0x0201ff020000006869020000000a0b01020003010000007a010000c03f\n"
                          "258 -1 hi 0x0a0b 0x0102 null [3, \"z\"] true 1.5\n16 5\n"));
    auto bad = run(decls + "network n { genesis { let d = decode(Acc, hex(\"0000000000000000\")); } }");
    CHECK(bad->has("does not start with the anchor discriminator"));
}

TEST_CASE("step budget stops runaway loops", "[interp][slow]") {
    auto r = run("network n { genesis { let i = 0; while true { i += 1; } } }");
    CHECK(r->has("execution step limit exceeded"));
}

// ------------------------------------------- regressions (code review) --

TEST_CASE("malformed IDLs produce diagnostics, not crashes", "[interp]") {
    fs::path dir = temp_dir("tide_bad_idl_test");
    std::ofstream(dir / "p.so") << "x";
    std::ofstream(dir / "no_type.json") << R"({"instructions": [], "accounts": [{"name": "Counter"}],
        "types": [{"name": "Counter", "type": {"kind": "struct", "fields": [{"name": "value"}]}}]})";
    std::ofstream(dir / "bad_array.json") << R"({"instructions": [], "accounts": [{"name": "Counter"}],
        "types": [{"name": "Counter", "type": {"kind": "struct", "fields": [{"name": "v", "type": {"array": ["u8"]}}]}}]})";
    std::ofstream(dir / "bad_disc.json") << R"({"instructions": [], "accounts": [{"name": "Counter", "discriminator": [1,2,3,4,5,6,7,300]}],
        "types": [{"name": "Counter", "type": {"kind": "struct", "fields": []}}]})";
    for (const auto& [file, message] : std::vector<std::pair<std::string, std::string>>{
             {"no_type.json", "has no \"type\""}, {"bad_array.json", "must be [type, length]"}, {"bad_disc.json", "discriminator byte out of range"}}) {
        INFO(file);
        auto r = run("keypair p = seed(\"p\");\nnetwork n { genesis {\n  program p { id = p.pubkey; binary = \"p.so\"; idl = \"" + file +
                         "\"; }\n  account seed(\"s\").pubkey { lamports = 1 SOL; data = anchor(\"Counter\", {}); }\n} }",
                     {}, dir / "main.tid");
        CHECK_FALSE(r->result.ok);
        CHECK(r->has(message));
    }
    fs::remove_all(dir);
}

TEST_CASE("library exceptions in a scenario fail only that scenario", "[interp]") {
    auto r = run(R"(
        keypair a = seed("a");
        network n {
          scenario first { transaction { signers = [a]; instruction memo_program() { data = memo("x"); } } }
          scenario second { print("second ran"); }
        }
    )", {}, "test.tid", [](FakeBackend& b) {
        b.on_tx = [](const TransactionRequest&) -> TxResult { throw std::runtime_error("RPC connection refused"); };
    });
    INFO(r->dump());
    CHECK(r->result.ok);
    CHECK(r->result.scenarios_failed == 1);
    CHECK(r->out.str().find("second ran") != std::string::npos);
    CHECK(r->has("RPC connection refused"));
    CHECK(r->diags.all().front().loc);  // positioned at the transaction
    CHECK(r->backend.events.back() == "detach");
}

TEST_CASE("decoding untrusted lengths is bounded", "[interp]") {
    auto r = run("schema E using borsh {}\nschema S using borsh { items: [E]; }\n"
                 "network n { genesis { let s = decode(S, hex(\"ffffffff\")); } }");
    CHECK(r->has("exceeds the available data"));
    auto r2 = run("schema S using borsh { xs: [u64]; }\nnetwork n { genesis { let s = decode(S, hex(\"02000000\")); } }");
    CHECK(r2->has("exceeds the available data"));
}

TEST_CASE("fixed-length lists, bytes/[u8] interop and checked conversions", "[interp]") {
    CHECK(eval_print("schema S using borsh { a: u8; b: u8; }",
                     "print(hex(\"0102\") == [1, 2], [1, 2] == hex(\"0102\"), decode(S, [7, 8]).b);")
              .starts_with("true true 8\n"));
    auto len = run("fn f(x: [u8; 2]) -> int { return len(x); }\nnetwork n { genesis { let x = f(hex(\"010203\")); } }");
    CHECK(len->has("expected 2 elements, found 3"));
    CHECK(run("network n { genesis { let d = 1 ms * 18446744073709551617; } }")->has("duration overflow"));
    CHECK(run("network n { genesis { let x = sol(10000000000000000000000000000000.0 * 10000000000.0); } }")->has("out of range"));
}
