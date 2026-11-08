// Parser and semantic-checker tests: everything that must be rejected (or
// accepted) before any execution happens.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ast.hpp"
#include "diag.hpp"
#include "parser.hpp"
#include "sema.hpp"

namespace ast = tide::ast;
namespace fs = std::filesystem;
namespace sema = tide::sema;
using tide::Diagnostics;
using tide::is_reserved_keyword;
using tide::parse_document;
using tide::Severity;
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

struct Compiled {
    SourceManager sm;
    Diagnostics diags{sm};
    std::optional<ast::Document> doc;
    std::unique_ptr<sema::Program> prog;

    [[nodiscard]] bool ok() const { return prog != nullptr && !diags.has_errors(); }

    [[nodiscard]] std::string dump() const {
        std::string s;
        for (const auto& d : diags.all()) s += diags.format(d);
        return s;
    }

    // True if some diagnostic of `sev` contains `text` and (if given) sits on `line`.
    [[nodiscard]] bool has(std::string_view text, int line = -1, Severity sev = Severity::error) const {
        return std::ranges::any_of(diags.all(), [&](const auto& d) {
            if (d.severity != sev || d.message.find(text) == std::string::npos) return false;
            return line < 0 || (d.loc && static_cast<int>(sm.locate(d.loc.ptr).line) == line);
        });
    }

    // True if some error carries a note containing `text` on `line`.
    [[nodiscard]] bool has_note(std::string_view text, int line) const {
        return std::ranges::any_of(diags.all(), [&](const auto& d) {
            return std::ranges::any_of(d.notes, [&](const auto& n) {
                return n.second.find(text) != std::string::npos && n.first &&
                       static_cast<int>(sm.locate(n.first.ptr).line) == line;
            });
        });
    }
};

std::unique_ptr<Compiled> compile(const std::string& src, fs::path path = "test.tid") {
    auto c = std::make_unique<Compiled>();
    const SourceFile* f = c->sm.add(std::move(path), src);
    c->doc = parse_document(*f, c->diags);
    if (auto& doc = c->doc) c->prog = sema::analyze(*doc, c->sm, c->diags);
    return c;
}

std::unique_ptr<Compiled> compile_file(const fs::path& path) {
    auto c = std::make_unique<Compiled>();
    const SourceFile* f = c->sm.load(path);
    REQUIRE(f);
    c->doc = parse_document(*f, c->diags);
    if (auto& doc = c->doc) c->prog = sema::analyze(*doc, c->sm, c->diags);
    return c;
}

constexpr std::string_view examples_dir = TIDE_EXAMPLES_DIR;

}  // namespace

// ------------------------------------------------------------ examples --

TEST_CASE("design-document examples parse", "[parser]") {
    for (const char* name : {"01_multi_validator", "02_prefund", "03_spl_mint", "04_fork_mainnet", "05_scenarios",
                             "06_local_defi", "07_e2e_tokens"}) {
        INFO(name);
        SourceManager sm;
        Diagnostics d(sm);
        const SourceFile* f = sm.load(fs::path(examples_dir) / (std::string(name) + ".tid"));
        REQUIRE(f);
        CHECK(parse_document(*f, d).has_value());
        CHECK_FALSE(d.has_errors());
    }
}

TEST_CASE("design-document examples: semantic validation", "[sema]") {
    for (const char* name : {"01_multi_validator", "02_prefund", "03_spl_mint", "07_e2e_tokens"}) {
        INFO(name);
        auto c = compile_file(fs::path(examples_dir) / (std::string(name) + ".tid"));
        INFO(c->dump());
        CHECK(c->ok());
    }
    // A placeholder address is caught statically.
    auto fork = compile_file(fs::path(examples_dir) / "04_fork_mainnet.tid");
    CHECK(fork->has("invalid pubkey", 5));
    // Live-network features are reserved for a later version.
    auto sc = compile_file(fs::path(examples_dir) / "05_scenarios.tid");
    CHECK(sc->has("'snapshot' is reserved", 3));
    CHECK(sc->has("'warp' is reserved", 5));
    CHECK(sc->has("'restore' is reserved", 7));
    auto defi = compile_file(fs::path(examples_dir) / "06_local_defi.tid");
    CHECK(defi->diags.error_count() == 1);
    CHECK(defi->has("'snapshot' is reserved", 54));
}

// --------------------------------------------------------------- parser --

TEST_CASE("syntax errors carry precise positions", "[parser]") {
    auto c = compile("keypair a = seed(\"a\")\nnetwork n { }\n");
    CHECK(c->has("expected ';'", 1));
    CHECK(compile("let account = 5;")->has("'account' is a reserved keyword"));
    // Both ends of the reserved list, and a contextual keyword that stays usable.
    CHECK(compile("let import = 5;")->has("'import' is a reserved keyword"));
    CHECK(compile("let extension = 5;")->has("'extension' is a reserved keyword"));
    CHECK(compile("let slot = 5;")->ok());
    CHECK(is_reserved_keyword("extension"));
    CHECK_FALSE(is_reserved_keyword("slot"));
    CHECK(compile("let s = \"abc\n\";")->has("unterminated string literal"));
    CHECK(compile("let r = 1..2..3;")->has("operator cannot be chained"));
    CHECK(compile("account x { }")->has("expected a declaration"));
    CHECK(compile("network n { bootstrap { warp to moon 5; } }")->has("expected 'to slot'"));
    CHECK(compile("network n { genesis { feature pubkey(\"x\") = on; } }")->has("expected 'enabled' or 'disabled'"));
    CHECK(compile("let x = (1 + ;")->has("expected an expression"));
    CHECK(compile("/* never closed")->has("expected '*/'"));
}

TEST_CASE("string literal locations span the source text, quotes and escapes included", "[parser]") {
    for (const std::string_view literal : {R"("hello")", R"("")", R"("a\nb\"c")", R"("café \\")"}) {
        auto c = compile("let s = " + std::string(literal) + ";");
        REQUIRE(c->ok());
        const auto& let = std::get<ast::LetStmt>(must(c->doc).decls[0].node);
        CHECK(std::string_view(let.init->loc.ptr, let.init->loc.len) == literal);
    }
}

TEST_CASE("operator precedence and associativity", "[parser]") {
    auto c = compile("let x = 1 + 2 * 3 - 4 == 3 && !false || false;");
    REQUIRE(c->ok());
    const auto& let = std::get<ast::LetStmt>(must(c->doc).decls[0].node);
    const auto* top = let.init->as<ast::BinaryExpr>();
    REQUIRE(top);
    CHECK(top->op == ast::BinaryOp::or_);
    const auto* conj = top->lhs->as<ast::BinaryExpr>();
    REQUIRE(conj);
    CHECK(conj->op == ast::BinaryOp::and_);
    const auto* eq = conj->lhs->as<ast::BinaryExpr>();
    REQUIRE(eq);
    CHECK(eq->op == ast::BinaryOp::eq);
    const auto* sub = eq->lhs->as<ast::BinaryExpr>();  // (1 + 2*3) - 4
    REQUIRE(sub);
    CHECK(sub->op == ast::BinaryOp::sub);
    const auto* add = sub->lhs->as<ast::BinaryExpr>();
    REQUIRE(add);
    CHECK(add->op == ast::BinaryOp::add);
    const auto* mul = add->rhs->as<ast::BinaryExpr>();
    REQUIRE(mul);
    CHECK(mul->op == ast::BinaryOp::mul);
}

TEST_CASE("comments, trailing commas, else-if and compound assignment parse", "[parser]") {
    auto c = compile(R"(
        // line comment
        /* block
           comment */
        let xs = [1, 2, 3,];
        let o = {a: 1, "b c": [1,], };
        fn f(x: int) -> int {
          let y = x;
          if y > 2 { y -= 1; } else if y < 0 { y *= -1; } else { y %= 7; }
          return y;
        }
    )");
    INFO(c->dump());
    CHECK(c->ok());
}

// ---------------------------------------------------------------- units --

TEST_CASE("numeric literals and units fold exactly", "[sema]") {
    auto c = compile("let a = 1.5 SOL; let b = 2 s; let c = 250 ms; let d = 7 lamports; let e = 0.25;");
    REQUIRE(c->ok());
    auto num = [&](std::size_t i) {
        return std::get<ast::LetStmt>(must(c->doc).decls[i].node).init->as<ast::NumberLit>();
    };
    CHECK(num(0)->int_value == 1'500'000'000);
    CHECK(num(1)->int_value == 2000);
    CHECK(num(2)->int_value == 250);
    CHECK(num(3)->int_value == 7);
    CHECK_FALSE(num(4)->folded_int);
    CHECK(compile("let x = 0.0000000001 SOL;")->has("not a whole number of lamports"));
    CHECK(compile("let x = 1.5 lamports;")->has("not a whole number of lamports"));
    CHECK(compile("let x = 99999999999999999999999999999999999999999;")->has("too large"));
    CHECK(compile("let x: u8 = 256;")->has("out of range for u8"));
    CHECK(compile("let d = 1 s + 5;")->has("cannot be applied to duration and int"));
}

// -------------------------------------------------------------- scoping --

TEST_CASE("symbol resolution and scoping", "[sema]") {
    CHECK(compile("let a = 1; let a = 2;")->has("redeclaration of 'a'"));
    CHECK(compile("let a = b; let b = 1;")->has("unknown identifier 'b'"));
    CHECK(compile("keypair alice = seed(\"a\"); let x = alcie;")->has("did you mean 'alice'?"));
    // Block scopes end with the block; inner blocks may shadow.
    auto c = compile(R"(
        fn f() -> int {
          let x = 1;
          if true { let x = "inner"; let y = 2; }
          return y;
        }
    )");
    CHECK(c->has("unknown identifier 'y'", 5));
    CHECK_FALSE(c->has("redeclaration"));
    // Functions see only earlier top-level declarations and their parameters.
    CHECK(compile("fn f() -> int { return later; } let later = 1;")->has("unknown identifier 'later'"));
    CHECK(compile("fn f() -> int { return g(); } fn g() -> int { return 1; }")->has("unknown function 'g'"));
    CHECK(compile("fn fact(n: int) -> int { if n <= 1 { return 1; } return n * fact(n - 1); }")->ok());
    CHECK(compile("network n { let local = 1; } fn f() -> int { return local; }")->has("unknown identifier 'local'"));
    // Genesis locals do not leak into bootstrap.
    CHECK(compile("network n { genesis { let g = 1; } bootstrap { let h = g; } }")->has("unknown identifier 'g'"));
}

TEST_CASE("mutability rules", "[sema]") {
    CHECK(compile("keypair k = seed(\"k\"); fn f() { k = seed(\"x\"); }")->has("cannot assign to keypair 'k'"));
    CHECK(compile("fn f(p: int) { p = 2; }")->has("cannot assign to parameter 'p'"));
    CHECK(compile("fn f() { for i in 0..3 { i = 1; } }")->has("cannot assign to loop variable 'i'"));
    CHECK(compile("let g = 1; network n { genesis { g = 2; } }")->has("cannot assign to top-level variable 'g'"));
    CHECK(compile("fn f() { let o = {a: 1}; o.a = 2; o.b = 3; }")->has("has no member 'b'"));
    CHECK(compile("fn f() { 1 + 1 = 2; }")->has("invalid assignment target"));
    CHECK(compile("fn f() { let s = \"a\"; s += 1; }")->has("compound assignment cannot combine string with int"));
}

// ---------------------------------------------------------------- types --

TEST_CASE("static type checking", "[sema]") {
    CHECK(compile("let x = [1, \"a\"];")->has("incompatible types int and string"));
    CHECK(compile("let x = null;")->has("cannot infer the type"));
    CHECK(compile("let x: Option<pubkey> = null;")->ok());
    CHECK(compile("let x: [u8; 2] = [1, 2]; let y: bytes = x;")->ok());
    CHECK(compile("let x: [u8; 4] = [1, 2, 3];")->has("expected 4 elements for [u8; 4], found 3"));
    CHECK(compile("let x: Strng = \"a\";")->has("unknown type 'Strng'; did you mean 'string'?"));
    CHECK(compile("let x: (u8, u8) = [1, 2];")->has("tuple types are only allowed in schema fields"));
    CHECK(compile("let x = seed(\"a\").pubkey.bytes;")->has("type pubkey has no member 'bytes'"));
    CHECK(compile("let x = 1; let y = x(2);")->has("'x' is not a function"));
    CHECK(compile("fn f(a: int) -> int { return a; } let y = f(1, 2);")->has("expects 1 argument(s), found 2"));
    CHECK(compile("fn f(a: pubkey) {} let y = f(1);")->has("expected pubkey, found int"));
    CHECK(compile("fn f() -> int { if true { return 1; } }")->has("may reach the end of its body"));
    CHECK(compile("fn f() -> int { if true { return 1; } else { return 2; } }")->ok());
    CHECK(compile("let x = 1 == \"a\";")->has("cannot be applied to int and string"));
    CHECK(compile("let x = seed(\"a\") == seed(\"b\");")->has("cannot be applied to keypair and keypair"));
    CHECK(compile("let x = sha256(5);")->has("expected string or bytes"));
    CHECK(compile("let x = pubkey(\"11111111111111111111111111111112\");")->ok());
    CHECK(compile("let x = hex(\"abc\");")->has("invalid hex string"));
    CHECK(compile("let x = seed;")->has("must be called"));
    CHECK(compile("let x = pda(system_program(), [\"a\", 1]);")->has("seeds must be string, bytes or pubkey"));
    CHECK(compile("let x = pda(system_program(), [\"this seed is definitely longer than 32 bytes\"]);")
              ->has("the maximum is 32"));
}

TEST_CASE("schemas validate object literals statically", "[sema]") {
    const std::string schema =
        "schema Counter using anchor { value: u64; owner: pubkey; tag: Option<u8>; pair: (u8, string); }\n";
    CHECK(compile(schema + "let d = borsh(Counter, {value: 1, owner: seed(\"a\").pubkey, pair: [1, \"x\"]});")->ok());
    auto c = compile(schema + "let d = borsh(Counter, {valu: 1, owner: 2, pair: [1]});");
    CHECK(c->has("unknown field 'valu' for schema 'Counter'; did you mean 'value'?"));
    CHECK(c->has("expected pubkey, found int"));
    CHECK(c->has("expected 2 elements"));
    CHECK(c->has("missing field 'value'"));
    CHECK(compile(schema +
                  "let d = borsh(Counter, {value: 300000000000000000000, owner: seed(\"a\").pubkey, pair: [1, \"\"]});")
              ->has("out of range for u64"));
    CHECK(compile("schema S using json { a: u8; }")->has("unknown schema format 'json'"));
    CHECK(compile("schema S using borsh { a: int; }")->has("sized integer type"));
    CHECK(compile("schema S using borsh { a: keypair; }")->has("cannot be used in a schema"));
    CHECK(compile("schema S using borsh { a: u8; a: u16; }")->has("duplicate field 'a'"));
    CHECK(compile(schema + "let v = decode(Counter, hex(\"\")).value + 1;")->ok());
    CHECK(compile(schema + "let v = decode(Counter, hex(\"\")).nope;")->has("has no member 'nope'"));
}

// ------------------------------------------------------------- contexts --

TEST_CASE("statement contexts are enforced", "[sema]") {
    CHECK(compile("network n { genesis { airdrop seed(\"a\").pubkey amount 1 SOL; } }")
              ->has("'airdrop' is a runtime statement and is only allowed inside bootstrap or scenario"));
    CHECK(compile("network n { bootstrap { account seed(\"a\").pubkey { lamports = 1; } } }")
              ->has("'account' declarations are only allowed inside genesis"));
    CHECK(compile("network n { genesis { return; } }")->has("'return' outside of a function"));
    CHECK(compile("network n { genesis { break; } }")->has("'break' outside of a loop"));
    CHECK(compile("network n { genesis { keypair k = seed(\"k\"); } }")
              ->has("only allowed at top level or directly inside a network"));
    CHECK(compile("let b = balance(seed(\"a\").pubkey);")->has("reads network state"));
    CHECK(compile("network n { genesis { let b = balance(seed(\"a\").pubkey); } }")->ok());
    CHECK(compile("network n { genesis { let s = slot(); } }")->has("needs a running network"));
    CHECK(compile("network n { options { faucet_sol = balance(seed(\"a\").pubkey); } }")->has("reads network state"));
    CHECK(compile("network n { scenario s { let s2 = slot(); await slot() > 10; } }")->ok());
    CHECK(compile("network n { genesis { 1 + 2; } }")->has("expression result is unused", -1, Severity::warning));
}

TEST_CASE("functions can be called wherever everything they do is allowed", "[sema]") {
    const std::string fns = R"(keypair k = seed("k");
fn reads() -> u64 { return balance(k.pubkey); }
fn writes() { mint k.pubkey { decimals = 6; } }
fn sends() { airdrop k.pubkey amount 1 SOL; }
fn waits() { await slot() > 1; }
fn pure_fn(n: int) -> int { return n + 1; }
)";
    auto in = [&](const std::string& block, const std::string& body) {
        return compile(fns + "network n { " + block + " {\n" + body + "\n} }");
    };
    // Bodies may now contain any operation; only the call sites are checked.
    CHECK(compile(fns)->ok());
    CHECK(in("genesis", "let b = reads(); writes(); let p = pure_fn(1);")->ok());
    CHECK(in("bootstrap", "let b = reads(); sends(); waits();")->ok());
    CHECK(in("scenario s", "let b = reads(); sends(); waits();")->ok());
    CHECK(compile(fns + "let top = pure_fn(1);\nnetwork n { let net = pure_fn(2); }")->ok());

    // Each kind of operation is rejected where it is not allowed, at the call and with the responsible line.
    auto bad = in("genesis", "sends();");
    CHECK(bad->has("'sends' sends a transaction and cannot be called in genesis, where no network is running yet", 8));
    CHECK(bad->has_note("'airdrop' sends a transaction here", 4));
    auto early = in("genesis", "waits();");
    CHECK(early->has("'waits' needs a running network and cannot be called in genesis", 8));
    CHECK(early->has_note("'await' needs a running network here", 5));
    auto late = in("bootstrap", "writes();");
    CHECK(late->has("'writes' declares genesis state and can only be called inside genesis", 8));
    CHECK(late->has_note("'mint' declares genesis state here", 3));
    auto top = compile(fns + "let b = reads();");
    CHECK(top->has("'reads' reads network state and can only be called inside genesis, bootstrap or scenario", 7));
    CHECK(top->has_note("'balance' reads network state here", 2));
    CHECK(compile(fns + "network n { let b = reads(); }")->has("'reads' reads network state"));
    CHECK(compile(fns + "network n { options { faucet_sol = reads(); } }")->has("'reads' reads network state"));
}

TEST_CASE("operations add up through calls and are explained along the call chain", "[sema]") {
    const std::string fns = R"(keypair k = seed("k");
fn deposit() { airdrop k.pubkey amount 1 SOL; }
fn check() -> bool { return balance(k.pubkey) > 0; }
fn setup() {
  deposit();
  assert check();
}
fn outer() { setup(); }
)";
    CHECK(compile(fns + "network n { bootstrap { outer(); } scenario s { setup(); } }")->ok());
    auto c = compile(fns + "network n { genesis {\nouter();\n} }");
    CHECK(c->has("'outer' sends a transaction and cannot be called in genesis", 10));
    CHECK(c->has_note("'outer' calls 'setup' here", 8));
    CHECK(c->has_note("'setup' calls 'deposit' here", 5));
    CHECK(c->has_note("'airdrop' sends a transaction here", 2));
    // Only the first violated kind is reported, once.
    CHECK(c->diags.error_count() == 1);
    CHECK(compile(fns + "let x = outer();")->has("'outer' reads network state"));

    // Recursion does not loop the analysis, and a recursive function still carries its operations.
    const std::string rec =
        "keypair k = seed(\"k\");\nfn drip(n: int) { if n > 0 { airdrop k.pubkey amount 1; drip(n - 1); } }\n";
    CHECK(compile(rec + "network n { bootstrap { drip(3); } }")->ok());
    CHECK(compile(rec + "network n { genesis { drip(3); } }")->has("'drip' sends a transaction"));
}

TEST_CASE("functions that both declare genesis state and need the network are rejected at the definition", "[sema]") {
    auto c = compile(R"(keypair k = seed("k");
fn seed_it() { mint k.pubkey { decimals = 6; } }
fn both() {
  seed_it();
  airdrop k.pubkey amount 1;
}
network n { genesis { both(); } bootstrap { both(); } }
)");
    CHECK(c->has("function 'both' declares genesis state and sends a transaction, so it cannot be called anywhere", 3));
    CHECK(c->has_note("'both' calls 'seed_it' here", 4));
    CHECK(c->has_note("'mint' declares genesis state here", 2));
    CHECK(c->has_note("'airdrop' sends a transaction here", 5));
    // Its call sites add nothing: the definition already explains the problem.
    CHECK(c->diags.error_count() == 1);
    CHECK(compile("fn f() { account seed(\"a\").pubkey { lamports = 1; } let s = slot(); }")
              ->has("declares genesis state and needs a running network"));
}

TEST_CASE("program labels are unique per body, not across function calls", "[sema]") {
    const std::string fn = "fn add(id: pubkey) { program vault { id = id; binary = \"v.so\"; } }\n";
    CHECK(compile(fn + "network n { genesis { add(seed(\"a\").pubkey); add(seed(\"b\").pubkey); } }")->ok());
    CHECK(compile(fn + "network n { genesis { add(seed(\"a\").pubkey);\n"
                       "program vault { id = seed(\"b\").pubkey; binary = \"v.so\"; } } }")
              ->ok());
    CHECK(compile("fn f(a: pubkey, b: pubkey) { program p { id = a; binary = \"x.so\"; } "
                  "program p { id = b; binary = \"x.so\"; } }")
              ->has("duplicate program 'p'"));
}

TEST_CASE("network structure validation", "[sema]") {
    CHECK(compile("network a {} network a {}")->has("duplicate network 'a'"));
    CHECK(compile("network a { genesis {} genesis {} }")->has("already has a genesis block"));
    CHECK(compile("network a { options {} options {} }")->has("already has an options block"));
    CHECK(compile("network a { bootstrap {} bootstrap {} }")->has("already has a bootstrap block"));
    CHECK(compile("network a { scenario s {} scenario s {} }")->has("duplicate scenario 's'"));
    CHECK(compile("network a { validator v {} validator v {} }")->has("duplicate validator 'v'"));
    CHECK(compile("network a { options { slot_per_epoch = 1; } }")->has("did you mean 'slots_per_epoch'?"));
    CHECK(compile("network a { options { startup_timeout = 5; } }")->has("expected duration, found int"));
    CHECK(compile("network a { validator v { rpc_port = 70000; } }")->has("out of range for u16"));
}

// --------------------------------------------------- account validation --

TEST_CASE("genesis declarations are validated before execution", "[sema]") {
    auto net = [](const std::string& body) {
        return "keypair k = seed(\"k\");\nnetwork n { genesis {\n" + body + "\n} }";
    };
    CHECK(compile(net("account k.pubkey { owner = system_program(); }"))
              ->has("missing required field 'lamports' in account"));
    CHECK(compile(net("account k.pubkey { lamports = 1; lamport = 2; }"))->has("did you mean 'lamports'?"));
    CHECK(compile(net("account k.pubkey { lamports = 1; lamports = 2; }"))->has("duplicate field 'lamports'"));
    CHECK(compile(net("account k { lamports = 1; }"))->has("expected pubkey, found keypair"));
    CHECK(compile(net("account k.pubkey { lamports = -1; }"))->has("out of range for u64"));
    CHECK(compile(net("account k.pubkey { lamports = 1; data = \"raw\"; }"))->has("expected bytes, found string"));
    CHECK(compile(net("account k.pubkey { lamports = 1; executable = true; }"))
              ->has("executable account is owned by the system program", -1, Severity::warning));
    CHECK(compile(net("mint k.pubkey { decimals = 256; }"))->has("out of range for u8"));
    CHECK(compile(net("mint k.pubkey { supply = 1; }"))->has("missing required field 'decimals'"));
    CHECK(
        compile(net("mint k.pubkey { decimals = 6; mint_authority = 5; }"))->has("expected Option<pubkey>, found int"));
    CHECK(compile(net("mint k.pubkey { decimals = 6; extension transfer_hook {} }"))
              ->has("unknown mint extension 'transfer_hook'"));
    CHECK(compile(net("mint k.pubkey { decimals = 6; extension mint_close_authority { close_authority = null; } }"))
              ->ok());
    CHECK(compile(net("associated_token_account k.pubkey { mint = k.pubkey; owner = k.pubkey; extension "
                      "immutable_owner {} }"))
              ->has("extensions are not supported in associated_token_account"));
    CHECK(compile(net("token_account k.pubkey { mint = k.pubkey; owner = k.pubkey; state = \"locked\"; }"))
              ->has("must be \"initialized\" or \"frozen\""));
    CHECK(compile(net("program p { id = k.pubkey; }"))->has("missing required field 'binary'"));
    CHECK(
        compile(net("program p { id = k.pubkey; binary = \"a.so\"; } program p { id = k.pubkey; binary = \"b.so\"; }"))
            ->has("duplicate program 'p'"));
    CHECK(compile(net("sysvar clok { slot = 1; }"))->has("did you mean 'clock'?"));
    CHECK(compile(net("sysvar clock { slots = 1; }"))->has("did you mean 'slot'?"));
    CHECK(compile(net("clone program k.pubkey from \"devnet\" { lamports = 1; };"))
              ->has("overrides are only supported when cloning accounts"));
    CHECK(compile(net("lookup_table k.pubkey { addresses = [k]; }"))->has("expected pubkey, found keypair"));
}

TEST_CASE("runtime statement validation", "[sema]") {
    auto net = [](const std::string& body) {
        return "keypair k = seed(\"k\");\nnetwork n { bootstrap {\n" + body + "\n} }";
    };
    CHECK(compile(net("transaction { signers = [k]; }"))->has("transaction has no instructions"));
    CHECK(compile(net("transaction { signer = [k]; instruction system_program() { data = hex(\"\"); } }"))
              ->has("did you mean 'signers'?"));
    CHECK(compile(net("transaction { signers = [k.pubkey]; instruction system_program() {} }"))
              ->has("expected keypair, found pubkey"));
    CHECK(compile(net("transaction { signers = [k]; instruction system_program() { accounts = [k]; } }"))
              ->has("expected account_meta, found keypair"));
    CHECK(compile(net("transaction as t { signers = [k]; instruction system_program() { accounts = [k.pubkey, "
                      "readonly(k.pubkey)]; } }\n"
                      "assert t.success && t.error == \"\";"))
              ->ok());
    CHECK(compile(net("invoke k.pubkey with { args = {}; };"))->has("missing required field 'method' in invoke"));
    CHECK(compile(net("deploy k.pubkey from \"p.so\";"))->has("expected keypair, found pubkey"));
    CHECK(compile(net("airdrop k.pubkey amount 1.5;"))->has("expected u64, found float"));
    CHECK(compile(net("await 1;"))->has("expected bool, found int"));
}

// --------------------------------------------------------------- imports --

TEST_CASE("imports resolve module members and detect cycles", "[sema]") {
    const fs::path dir = fs::temp_directory_path() / "tide_import_test";
    fs::create_directories(dir);
    std::ofstream(dir / "lib.tid")
        << "keypair admin = seed(\"admin\");\nfn lamports_of(n: int) -> int { return n * 1 SOL; }\n"
           "schema Pair using borsh { a: u8; b: u8; }\n";
    std::ofstream(dir / "main.tid")
        << "import \"lib.tid\" as lib;\n"
           "network n { genesis { account lib.admin.pubkey { lamports = lib.lamports_of(2); } } }\n";
    std::ofstream(dir / "typo.tid") << "import \"lib.tid\" as lib;\nlet x = lib.admn;\n";
    std::ofstream(dir / "a.tid") << "import \"b.tid\" as b;\n";
    std::ofstream(dir / "b.tid") << "import \"a.tid\" as a;\n";
    std::ofstream(dir / "missing.tid") << "import \"nope.tid\" as nope;\n";

    auto ok = compile_file(dir / "main.tid");
    INFO(ok->dump());
    CHECK(ok->ok());
    CHECK(ok->prog->init_order.size() == 2);
    CHECK(compile_file(dir / "typo.tid")->has("module 'lib' has no member 'admn'; did you mean 'admin'?"));
    CHECK(compile_file(dir / "a.tid")->has("import cycle"));
    CHECK(compile_file(dir / "missing.tid")->has("cannot open imported file"));
    fs::remove_all(dir);
}

TEST_CASE("library functions carry their operations across modules", "[sema]") {
    const fs::path dir = fs::temp_directory_path() / "tide_import_ops_test";
    fs::create_directories(dir);
    std::ofstream(dir / "vault.tid") << "fn deposit(user: keypair, amount: u64) {\n"
                                        "  airdrop user.pubkey amount amount;\n"
                                        "}\n";
    std::ofstream(dir / "dev.tid") << "import \"vault.tid\" as vault;\n"
                                      "keypair bob = seed(\"bob\");\n"
                                      "network n {\n"
                                      "  bootstrap { vault.deposit(bob, 50000000); }\n"
                                      "  scenario s { vault.deposit(bob, 1); }\n"
                                      "  genesis {\n"
                                      "    vault.deposit(bob, 1);\n"
                                      "  }\n"
                                      "}\n";
    auto c = compile_file(dir / "dev.tid");
    CHECK(c->has("'vault.deposit' sends a transaction and cannot be called in genesis", 7));
    CHECK(c->has_note("'airdrop' sends a transaction here", 2));
    CHECK(c->diags.error_count() == 1);
    fs::remove_all(dir);
}
