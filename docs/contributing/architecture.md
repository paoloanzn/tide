# Architecture and contributing

This page is for people who want to build Tide from source, find their way around the code, or extend it. Tide is licensed under the [Apache License 2.0](../../LICENSE); contributions are accepted under the same license. It covers the build, the source layout, the pipeline from source text to a running network, the backend boundary, diagnostics, the test suite, and short checklists for common changes.

## Building from source

### Requirements

| Requirement | Notes |
| --- | --- |
| CMake >= 3.25 | `cmake_minimum_required(VERSION 3.25)`; the presets file uses schema version 6. |
| Ninja | Both presets use the `Ninja` generator. |
| C++20 compiler | `CMAKE_CXX_STANDARD 20`, extensions off. Tide's `int` is a 128-bit integer (`tide::i128`, an alias of the `__int128` extension), so GCC or Clang is required. GCC 12 and 13 and Apple Clang (Xcode 16) are built in CI. |
| vcpkg | Dependencies come from `vcpkg.json` in manifest mode. `VCPKG_ROOT` must point at your vcpkg checkout; the presets load `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`. |
| POSIX system | The test-validator backend uses `posix_spawn`, signals and sockets. Linux and macOS are built and tested in CI. |
| clang-format, clang-tidy, clangd 18 | Optional for building; required for the `lint` preset and for keeping changes formatted and lint-clean (see [code style and linting](#code-style-and-linting)). |

Dependencies declared in `vcpkg.json`:

| vcpkg port | Used for | Linked into |
| --- | --- | --- |
| `foonathan-lexy` | Parser combinators for the grammar in `src/parser.cpp` | `tide_core` |
| `libsodium` | ed25519 keys and signatures | `tide_core` |
| `openssl` | SHA-256 and big-number arithmetic (`libcrypto`) | `tide_core` |
| `nlohmann-json` | IDL parsing, JSON values, manifests and account files | `tide_core` |
| `cpr` | HTTP client for JSON-RPC to the validator | `tide_solana_backend` only |
| `catch2` | Test framework (v3) | `tide_tests` only |

The vcpkg baseline is pinned with `builtin-baseline` in `vcpkg.json`. Installed packages go to `build/vcpkg_installed`, shared by both presets.

### Presets

`CMakePresets.json` defines three configure presets and matching build presets:

| Preset | Build type | Binary dir |
| --- | --- | --- |
| `default` | Debug | `build/debug` |
| `release` | Release | `build/release` |
| `lint` | Debug, with clang-tidy on every source and all warnings as errors | `build/lint` |

All of them set `VCPKG_FORCE_SYSTEM_BINARIES=1`, so vcpkg uses the CMake and Ninja found on your `PATH` instead of downloading its own. There is one test preset, `default`, bound to the Debug build, with output shown on failure.

### Building

```sh
export VCPKG_ROOT=~/vcpkg   # wherever vcpkg is checked out

cmake --preset default      # configure (first run builds the vcpkg dependencies)
cmake --build --preset default

cmake --preset release
cmake --build --preset release
```

This produces `build/<preset>/tide` (the CLI) and `build/<preset>/tide_tests`. The test executable is built by default; configure with `-DTIDE_BUILD_TESTS=OFF` to skip it (Catch2 is then not required at configure time).

The build defines three targets plus the tests:

| Target | Kind | Sources |
| --- | --- | --- |
| `tide_core` | static library | `parser.cpp`, `sema.cpp`, `interp.cpp`, `solana.cpp` |
| `tide_solana_backend` | static library | `solana_backend.cpp` (links `tide_core` and `cpr`) |
| `tide` | executable | `main.cpp` |
| `tide_tests` | executable | every `tests/*.cpp` (globbed) |

### Running the tests

Through CTest (each Catch2 `TEST_CASE` is registered as its own CTest test by `catch_discover_tests`):

```sh
ctest --preset default
```

Or run the Catch2 binary directly, optionally filtered by tag or name:

```sh
./build/debug/tide_tests             # everything
./build/debug/tide_tests "[sema]"    # one tag: [parser], [sema], [interp], [solana], [backend], [e2e]
```

The end-to-end test is opt-in, because it starts real validator processes and takes about 20 seconds. Without `TIDE_E2E` it reports as skipped (CTest lists it under "The following tests did not run").

```sh
TIDE_E2E=1 ./build/debug/tide_tests "[e2e]"
```

It needs:

- `solana-test-validator` on `PATH` (for example `PATH=$HOME/.local/bin:$PATH` if the Agave tools are installed there),
- the ports used by `tests/examples/07_e2e_tokens.tid` to be free: RPC 18899, faucet 19900, gossip 18001 and the dynamic range 18002-18040. They are deliberately not the defaults, so a validator you already run on 8899 does not collide.

The test writes its state under the system temp directory (`tide_e2e_state`), checks `getHealth` on the launched network, then stops it with `stop_network` and deletes the state.

### Code style and linting

The repository carries three configuration files at its root. A change is expected to leave all of them clean.

| File | Tool | What it enforces |
| --- | --- | --- |
| `.clang-format` | clang-format 18 | Google style adapted to the code: 4-space indent, 120 columns, leading `const`, includes grouped as own header, third-party, POSIX, standard library, project. |
| `.clang-tidy` | clang-tidy 18 | A strict profile: every `bugprone`, `cert`, `clang-analyzer`, `concurrency`, `cppcoreguidelines`, `google`, `misc`, `modernize`, `performance`, `portability` and `readability` check, a cognitive-complexity limit of 25 per function, include hygiene (`misc-include-cleaner`) and naming rules (snake_case functions and variables, CamelCase types, private members ending in `_`). The few disabled checks are listed with their reason at the top of the file. |
| `.clangd` | clangd 18 | Points clangd at `build/debug/compile_commands.json`, shows the clang-tidy findings and strict include diagnostics in the editor, and enables background indexing and inlay hints. |

Compiler warnings are set in `CMakeLists.txt` (`TIDE_WARNINGS`) for every target, tests included: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast` and more, plus GCC-only checks such as `-Wduplicated-branches` and `-Wuseless-cast`. Two options turn them into a gate:

| Option | Default | Effect |
| --- | --- | --- |
| `TIDE_WERROR` | `OFF` | Compiler warnings, and clang-tidy findings when `TIDE_CLANG_TIDY` is on, become errors. |
| `TIDE_CLANG_TIDY` | `OFF` | Runs clang-tidy with `.clang-tidy` on each source as it compiles. |

The `lint` preset enables both:

```sh
cmake --preset lint
cmake --build --preset lint        # fails on any warning or clang-tidy finding
```

Some `-Wnull-dereference` diagnostics only appear with optimisation, so build the `release` preset too before sending a change.

To format, or to lint without a full build:

```sh
clang-format -i src/*.cpp src/*.hpp tests/*.cpp
clang-format --dry-run -Werror src/*.cpp src/*.hpp tests/*.cpp   # check only
run-clang-tidy -p build/debug 'tide/(src|tests)/'
```

A finding is fixed rather than suppressed. Where a fix is impossible or clearly worse (a POSIX API that takes `sockaddr*`, state shared with a signal handler, `getenv`), the line carries `NOLINT(<check>)` or `NOLINTNEXTLINE(<check>)` naming the check, followed by the reason. Two regions in `src/parser.cpp` are exempt as a whole, and say why: the keyword X-macros, and the lexy grammar productions, which stay snake_case so they do not clash with the AST types they build (the production `stmt` builds an `ast::Stmt`).

## Continuous integration and releases

Two GitHub Actions workflows live in `.github/workflows/`.

`ci.yml` runs on every push and pull request. Its jobs mirror the local checks:

| Job | Runs on | Equivalent local command |
| --- | --- | --- |
| Format | Ubuntu 24.04 | `clang-format-18 --dry-run --Werror` on every tracked `.cpp` and `.hpp` |
| Lint | Ubuntu 24.04 | `cmake --preset lint && cmake --build --preset lint` |
| Test | Ubuntu 24.04 (x86_64), macOS 15 (arm64) | `cmake --preset default && cmake --build --preset default && ctest --preset default` |

`release.yml` builds the release binaries and publishes the GitHub release:

| Asset | Built on | Runs on |
| --- | --- | --- |
| `tide-linux-x86_64.tar.gz` | Ubuntu 22.04, GCC 12 | glibc 2.35 or newer |
| `tide-linux-arm64.tar.gz` | Ubuntu 22.04 arm64, GCC 12 | glibc 2.35 or newer |
| `tide-macos-arm64.tar.gz` | macOS 15, Apple Clang | macOS 14 or newer (`MACOSX_DEPLOYMENT_TARGET`) |

Each archive holds the stripped `tide` binary, `README.md`, `LICENSE`, `NOTICE` and `THIRD_PARTY_LICENSES.txt`, and has a `.sha256` file next to it. The vcpkg dependencies are linked statically, so the package step collects their licenses from vcpkg into `THIRD_PARTY_LICENSES.txt` and fails if one of the linked ports is missing. On Linux, `-static-libstdc++ -static-libgcc` link the C++ runtime in too, so the binary needs only glibc. Each build job runs the unit tests, checks that `tide --version` matches the tag, and runs `tide check` on an example. Once the builds and the whole CI workflow (called from `release.yml`) pass, the publish job creates the GitHub release for the tag, attaches the archives, their `.sha256` files and a combined `checksums.txt`, and uses GitHub's generated release notes. Re-running the workflow for a tag that already has a release replaces its assets.

Running `release.yml` by hand from the Actions tab (`workflow_dispatch`) is a dry run: everything is built and packaged and the archives are kept as run artifacts, but nothing is published.

Both workflows check out vcpkg at the `builtin-baseline` commit and cache the built packages with `actions/cache`, keyed by job, platform, `vcpkg.json` and runner image. A cold build of the dependencies takes several minutes; a cached one takes seconds.

`install.sh` at the repository root is the installer linked from the README. It finds the latest release through the `releases/latest` redirect, downloads `tide-<os>-<arch>.tar.gz` and its `.sha256`, verifies it, and replaces the binary atomically. The archive names carry no version, so the installer and the release workflow must keep the same names.

### Agent skill

`skills/tide/SKILL.md` is the official onboarding skill. It follows the
[Agent Skills specification](https://agentskills.io/specification), using
portable frontmatter and instructions rather than agent-specific tools.
Its workflow starts with prerequisite discovery, requires confirmation for
installation, and preserves the developer's existing project configuration.
The skill reads documentation as needed instead of embedding a second copy
of the language reference in its instructions.

`install-skill.sh` downloads a repository snapshot and packages the skill
with the complete `docs/` tree under `references/docs/`. It also includes
the root `README.md`, `LICENSE`, and `NOTICE` under `references/`, preserving
the documentation's relative links. `SOURCE` records the selected commit,
requested pin, fallback status, and Tide version from `CMakeLists.txt`.
The installer does not install Tide or Agave; that is the skill's job after
the developer approves the proposed commands.

The [README command](../../README.md#quick-start-with-an-ai-agent) downloads the
entire installer successfully before executing it. To inspect the script
and choose destinations explicitly:

```sh
curl --proto '=https' --proto-redir '=https' -fsSL \
  https://raw.githubusercontent.com/paoloanzn/tide/main/install-skill.sh \
  -o install-skill.sh
less install-skill.sh
sh install-skill.sh --agent claude --agent codex
```

Run these download commands outside a project with an existing
`install-skill.sh` if you want to keep that file. From a Tide checkout, run
the checked-out installer directly.

| Option or variable | Effect |
| --- | --- |
| No options | Register in the shared directory and detect other agents by their user directory or CLI executable. |
| `--agent NAME` | Select an agent explicitly; repeat for multiple agents. `all` selects all directories below. |
| `--skills-dir /absolute/path` | Register `/absolute/path/tide`; repeat for other destinations or agents. Explicit destinations replace automatic selection. |
| `--help` | Show usage without downloading or writing anything. |
| `TIDE_SKILL_COMMIT` | Override the embedded pin with a full, lowercase 40-character commit SHA. An empty value explicitly selects the main fallback. |
| `CLAUDE_CONFIG_DIR` | Use this Claude Code configuration directory instead of `~/.claude`. Must be absolute when selected. |

The installer maps agents to these personal skill directories:

| Agent names | Directory |
| --- | --- |
| `shared`, `codex`, `cursor`, `gemini`, `opencode`, `amp` | `~/.agents/skills` |
| `claude` | `${CLAUDE_CONFIG_DIR:-$HOME/.claude}/skills` |
| `copilot` | `~/.copilot/skills` |
| `cline` | `~/.cline/skills` |
| `kiro` | `~/.kiro/skills` |
| `antigravity` | `~/.gemini/config/skills` |

Each destination gets a real `tide/` directory: some agents do not discover
symlinked skill folders. The installer stages complete bundles for all selected
destinations before replacing any of them. It moves each previous bundle to a
temporary backup, then renames the new bundle into place. The directory can
briefly be absent between those two renames. If an activation fails, all changes
from that invocation are rolled back. Identical installations keep their files.

A `.tide-managed` marker identifies content owned by this installer. Updates
replace that content, including local edits, and remove the previous bundle.
Keep customizations in a separately named skill. Unrelated files, symlinks, or
skills at a destination cause installation to stop instead of being overwritten.
Every selected destination gets identical skill/docs contents; re-run with the
same options to update those copies together.

Downloads use HTTPS, including redirects, with bounded timeouts and retries.
Archive validation rejects traversal, links, special files, and member names
outside letters, digits, dots, underscores, hyphens, plus signs, `@`, and `/`.
Only the skill, docs, README, license, notice, and version metadata are extracted;
downloaded scripts are never executed. Commit pinning fixes source contents,
but does not independently authenticate the downloaded installer or GitHub.

Each skills directory has a temporary `.tide-install.lock` directory during an
update. If a process was killed without cleanup, verify that no installer is
still running before recovering it: `old/` contains the previous bundle, if any,
and `new/` contains the staged replacement, if it has not yet been activated.
Restore `old/` to `tide/` when needed, then remove the lock directory and retry.
Reload skills or restart the agent after an update. Installation on one machine
does not register the skill in a remote/cloud agent session.

To uninstall, delete the installed `tide/` directory at each selected location,
for example `rm -r ~/.agents/skills/tide`. This removes that copy of the skill
and bundled docs; other skills and the Tide binary are separate.

#### Publishing skill updates

1. Commit and publish the skill and documentation changes together.
2. Set `PINNED_COMMIT` in `install-skill.sh` to that published commit's full SHA,
   then publish the installer change. Prefer a snapshot compatible with a
   released Tide version. An installer cannot embed its own commit's hash.
3. Users re-run the same installation command to get the newly selected bundle.

For the first publication, the pin is empty because no published snapshot
contains the new skill yet. Advance it after that initial commit is published.
An absent, unavailable, or incomplete pinned snapshot produces a visible
fallback to `main` for **both** skill and docs. The installer resolves `main`
to a commit through GitHub's API when available; otherwise it downloads a
single branch archive and records `main (unresolved)`. If that fallback also
fails validation, the existing installation remains in place.

Validate changes with `shellcheck install-skill.sh`, `sh -n install-skill.sh`,
and `python3 tests/install_skill_test.py`. The tests use local archive responses
and temporary destinations, require no network or third-party Python packages,
and check installation, updates, fallback, archive rejection, collisions, and
failure recovery. CI runs them on Linux and macOS independently of the C++ build.
They verify installer behavior; onboarding instructions still need review for
correct approvals, supported Tide constructs, and useful project proposals.

### Cutting a release

The version is written in `project(tide VERSION ...)` in `CMakeLists.txt`. CMake passes it to the code as `TIDE_VERSION`, which `tide --version` prints. `vcpkg.json` has a `version-string` that must match, and the release workflow fails if it does not.

1. Update the version in `CMakeLists.txt`, `vcpkg.json`, and the prose that names it (`README.md`, `docs/README.md`, `docs/reference/cli.md`). Commit the change and push it to `main`.
2. Optionally run the Release workflow by hand on `main` as a dry run.
3. Tag the commit and push the tag:

   ```sh
   git tag -a v0.2.0 -m "Tide 0.2.0"
   git push origin v0.2.0
   ```

The tag must be `v` followed by the project version. A suffix (`v0.2.0-rc.1`) publishes a pre-release. The installer ignores pre-releases unless `TIDE_VERSION` names one. A tag that does not match the project version fails the workflow before anything is built.

## Code layout

| File | Role |
| --- | --- |
| `src/diag.hpp` | `Loc`, `SourceManager`, `Diagnostics`, `TideError`, and small shared utilities (`overloaded`, `Restore`, "did you mean" suggestions). Used by every stage. |
| `src/ast.hpp` | AST node types. Produced by the parser, decorated in place by sema (fields marked `// sema`), read by the interpreter. Also defines `BuiltinId`. |
| `src/parser.hpp`, `src/parser.cpp` | lexy grammar: source text to `ast::Document`. Holds the keyword lists. |
| `src/sema.hpp`, `src/sema.cpp` | Semantic analysis: modules and imports, symbol resolution, scoping, static types, statement-context rules, field specs, builtin table. Produces a `sema::Program`. |
| `src/interp.hpp`, `src/interp.cpp` | Direct AST interpreter: values, encoding (borsh/Anchor), builtins, statements, genesis construction and the lifecycle driver. |
| `src/state_builder.hpp` | The backend boundary: `NetworkBackend`, `GenesisBuilder`, `RuntimeClient` and the Solana-level request types. |
| `src/solana.hpp`, `src/solana.cpp` | Backend-agnostic Solana primitives: base58/base64/hex, keys, PDA and ATA derivation, well-known program ids, borsh reader/writer, SPL and lookup-table account layouts, legacy transaction serialization. No RPC, no processes, no AST. |
| `src/solana_backend.hpp`, `src/solana_backend.cpp` | `NetworkBackend` implementation driving Agave's `solana-test-validator`, plus `stop_network` for `tide down`. |
| `src/main.cpp` | CLI driver for `tide check`, `tide up` and `tide down` (see [CLI reference](../reference/cli.md)). |

## The pipeline

A script goes through four stages. Each stage finishes before the next one starts, and only the last one touches a network.

1. **Parse** (`parse_document`): text to AST. Syntax errors are reported and parsing returns `std::nullopt`.
2. **Analyze** (`sema::analyze`): resolve, type-check and validate the whole program, including every import. Returns `nullptr` if any error was reported. `tide check` stops here.
3. **Initialize globals** (`Interpreter::init_globals`): evaluate module-level bindings of every module, imports first.
4. **Run** (`Interpreter::run`): drive a `NetworkBackend` through the lifecycle for one network.

### Parser

`src/parser.cpp` expresses the grammar as lexy productions that build `ast` nodes directly. A custom error sink turns lexy errors into `Diagnostics` entries, attaching a missing `;`, `)`, `]` or `}` to the end of the previous token and suppressing cascades at the same position.

Keywords have a single source of truth, two X-macro lists:

```cpp
#define TIDE_RESERVED_KEYWORDS(X)                                     \
    X(import) X(as) X(source) X(keypair) X(let) X(schema) X(fn) ...
#define TIDE_CONTEXTUAL_KEYWORDS(X)                                   \
    X(using) X(from) X(with) X(at) X(slot) X(to) ...
```

Each list is expanded several times: into lexy keyword objects (`kw_<name>`), into the `reserved_words` array behind `is_reserved_keyword`, and into the identifier rule's reserved set. Reserved words can never be identifiers; contextual ones are keywords only where the grammar expects them (see [lexical structure](../language/lexical-structure.md)).

The grammar is deliberately lenient in places and leaves the check to sema, which gives better messages: all statement bodies share one statement grammar (sema enforces which statements are allowed in genesis, bootstrap, scenario and function bodies), and record keys and field names accept any word, including reserved ones such as `mint`.

### Semantic analysis

`src/sema.cpp` runs to completion before anything executes. It does not build a separate tree; it decorates the AST in place, and the interpreter relies on those decorations:

| Decoration | Where |
| --- | --- |
| Resolved symbol | `IdentExpr::sym`, `LetStmt::sym`, `ForStmt::sym`, `MemberExpr::module_member`, ... |
| Static type | `Expr::type` |
| Builtin id or user function | `CallExpr::builtin`, `CallExpr::function` |
| Folded literal value | `NumberLit` (units applied, e.g. `1 SOL` folds to an integer lamport count) |

The resulting `sema::Program` owns the type arena (`TypeContext`), all symbols, imported documents and modules (`modules[0]` is the root, `init_order` lists imports before importers).

Two tables in `sema.cpp` drive most validation:

- **`builtin_table`** maps each standard-library name to a `BuiltinId` and an optional `Op`: none for pure builtins, `Op::read` (reads network state; only in genesis, bootstrap or scenario) or `Op::live` (needs a running network; only in bootstrap or scenario). `check_builtin` then checks arity and argument types per id. The table is also the source for `builtin_name()` and "did you mean" suggestions. Note that the Tide name `account_info` maps to `BuiltinId::account`.
- **Network operations** (`Op`: read, write, send, live) also come from statements: genesis declarations are `write`, `transaction`/`invoke`/`deploy`/`airdrop` are `send`, `await` is `live`. In a block, each operation is checked against the block where it appears. In a function body, `record_op` records the first site of each kind instead (`OpSites`, kept per function in `fn_ops_`), including calls to functions that perform it. `check_call_ops` then checks a call against the callee's record, with the same placement rules, and `explain_op` follows the recorded calls to the responsible line for the notes. A function only sees functions declared before it, plus itself, so one pass is enough: callees are already recorded, and a self-call adds nothing. A function that both writes and sends (or is `live`) is reported at its definition by `report_uncallable`.
- **Field specs** (`FieldSpec` / `ExtSpec` / `BlockSpec`) list, for every record-like block, the accepted keys, their expected type (`Ty`) and whether they are required. There are specs for each genesis entity (`account_fields`, `mint_fields`, ...), Token-2022 extensions, sysvars, `options`, `validator`, `transaction`, `instruction` and `invoke`. Unknown keys, missing required keys and type mismatches are reported here, before execution.

### Interpreter

`src/interp.cpp` only reads the decorated AST. It never re-resolves names or re-checks types; when it finds something sema should have prevented, it throws an internal error rather than guessing.

`Interpreter::run` is the lifecycle driver. In order:

1. Evaluate network-level `let` bindings.
2. `configure(build_config(net))`: turn `options` and `validator` blocks into a `NetworkConfig`.
3. Execute every `genesis` block against `backend.genesis()`, then run cross-declaration checks (`validate_genesis`).
4. With `--dry-run`: `persist()` and stop.
5. If there is a `bootstrap` block: `start_bootstrap()` and execute it against the returned `RuntimeClient`.
6. `persist()`, then `launch()`.
7. Run each selected `scenario` against the launched network. A failing scenario is reported and counted but does not stop the others or the network.
8. `detach()`: the network keeps running after Tide exits.

Any exception before step 8 is reported as a diagnostic and `abort()` tears down whatever was started. State queries such as `balance` or `account_info` read from the `RuntimeClient` when one is active and from `GenesisBuilder::account` during genesis. The user-facing view of these phases is in [the lifecycle guide](../guides/lifecycle.md).

### The backend boundary

`src/state_builder.hpp` is the only way the interpreter talks to a network. It defines three interfaces:

| Interface | Purpose | Main methods |
| --- | --- | --- |
| `NetworkBackend` | Lifecycle | `configure`, `genesis`, `start_bootstrap`, `persist`, `launch`, `detach`, `abort` |
| `GenesisBuilder` | Privileged, pre-launch state writes | `put_account`, `put_program`, `clone_account`, `remove_account`, `set_feature`, `set_sysvar`, `fork`, `account` |
| `RuntimeClient` | Authorized interaction with a running network | `send_transaction`, `airdrop`, `deploy`, `account`, `slot`, `epoch` |

The values crossing it are Solana-level, not AST nodes: `NetworkConfig`/`ValidatorConfig`, `AccountSpec`, `ProgramSpec`, `SysvarSpec`, `FeatureSpec`, `ForkSpec`, `CloneSpec`, `DeleteSpec`, `TransactionRequest`, `DeployRequest`, and the results `AccountView`, `TxResult` and `LaunchInfo`. Each request carries an opaque source `Loc` (`origin`), so a backend can report a positioned error by throwing `TideError` without knowing anything about the script.

```cpp
struct TransactionRequest {
    std::vector<sol::Instruction> instructions;
    Pubkey fee_payer;
    std::vector<Keypair> signers;
    std::optional<std::uint32_t> compute_unit_limit;
    std::optional<std::uint64_t> compute_unit_price;
    Loc origin;
};
```

Why the boundary exists:

- **The core never depends on RPC or process management.** `tide_core` does not link `cpr` and contains no process code; the CMake file links the test-validator backend as a separate library for this reason.
- **Another backend implements the same interfaces.** A different validator, or an in-process bank, needs only a new `NetworkBackend`; the parser, checker and interpreter stay unchanged.
- **Tests run without a validator.** `tests/interp_test.cpp` defines `FakeBackend`, a single class implementing all three interfaces in memory, which records every call crossing the boundary. The interpreter tests assert on those recorded calls.

### The solana-test-validator backend

`src/solana_backend.cpp` implements `SolanaTestValidatorBackend`:

- **`Genesis`** (a `GenesisBuilder`) collects accounts and programs in memory. It refuses to overwrite builtin program accounts and reports a second write to the same address with a note pointing at the first. On `persist()` the state is written under `<state_root>/<network>/` as account JSON files plus program flags.
- **`JsonRpc`** is a JSON-RPC 2.0 transport (over `cpr`) to one endpoint, with a short-timeout `getHealth` probe used, together with `getSlot`, to wait until a starting validator is past slot 0.
- **`RpcRuntime`** (a `RuntimeClient`) sends transactions, airdrops and state queries over `JsonRpc`, and deploys programs through the `solana` CLI.
- **Process management**: bootstrap runs on a private validator instance started on the persisted state; `persist()` stops it after finalizing; `launch()` starts the long-running validator in a new session so it survives Tide; `stop_network()` (used by `tide down`) stops it later. Stops escalate to `SIGKILL` after a grace period. Until `launch()` returns, the running instance is recorded in an atomic that a SIGINT/SIGTERM/SIGHUP handler reads, so an interrupted `tide up` stops it instead of leaving an orphan with no pid file.

`SolanaBackendOptions::dry_run` writes state and prints the launch command without starting processes. Behaviour and limitations from a user's point of view are in [the solana-test-validator page](../backends/solana-test-validator.md).

### Solana primitives

`src/solana.{hpp,cpp}` (namespace `tide::sol`) holds everything Solana-specific that is neither AST nor transport: encodings, `Pubkey`/`Keypair` (including deterministic keys from seeds), `find_program_address`, associated token addresses, well-known program ids (`sol::ids`), `BorshWriter`/`BorshReader`, mint, token-account and lookup-table layouts with Token-2022 extensions, and legacy transaction serialization. Both the interpreter and the backend use it.

## Diagnostics plumbing

All stages share the types in `src/diag.hpp`:

| Type | Role |
| --- | --- |
| `Loc` | A raw pointer into a source buffer plus a length (0 means a point). Keeps AST nodes small and lets any stage point at source text without knowing the file. |
| `SourceManager` | Owns every loaded `SourceFile` (root and imports). `locate()` maps a `Loc` back to file, line, column and line text. |
| `Diagnostics` | Collects `Diagnostic`s (severity, `Loc`, message, notes) and renders `file:line:col: severity: message`, the source line and a caret underline, in colour when stderr is a terminal. |
| `TideError` | Exception thrown by the interpreter and backends for errors that abort the lifecycle. Carries a `Loc` and optional notes; `Interpreter` converts it into a `Diagnostic`. |

The parser and checker report directly into `Diagnostics` and continue, so one run shows many errors. The interpreter and backends throw `TideError` and stop at the first one. Anything thrown that is not a `TideError` is reported at a fallback location.

## Tests

| File | Covers |
| --- | --- |
| `tests/frontend_test.cpp` | Parser and checker: example scripts parse and validate, syntax error positions, precedence, literal folding, scoping, mutability, static types, schemas, statement contexts, network structure, genesis and runtime declaration checks, imports and cycles. Everything that must be rejected before execution. |
| `tests/interp_test.cpp` | Interpreter against `FakeBackend`: lifecycle order, dry run, scenario filtering, bootstrap failure, SPL and Token-2022 genesis state, cross-declaration validation, transactions and inferred account metas, Anchor `invoke` from an IDL, `await`, functions and control flow, copy semantics, exact arithmetic, positioned runtime errors, borsh schemas, step budget (`[slow]`), malformed IDLs, bounded decoding. |
| `tests/solana_test.cpp` | Solana primitives against vectors generated with solders (Python bindings of the Solana Rust SDK): base58, hex, base64, curve checks, PDAs, ATAs, seed keys, transaction serialization byte-for-byte, SPL layouts, borsh and Anchor discriminators. The generator recipe is at the bottom of the file. |
| `tests/e2e_test.cpp` | The opt-in `[e2e]` lifecycle against a real validator, and a `[backend]` dry-run test of the real backend (launch command flags, `manifest.json` and `accounts/` written). |

The **field-handler consistency test** ("every checker-accepted field has an interpreter handler", in `interp_test.cpp`) runs one script that sets every key accepted by the sema field specs for `options`, `validator`, every genesis entity, clone overrides, `transaction`, `instruction` and `invoke`. It catches drift between sema and the interpreter: the interpreter records which fields it read (`FieldVals::get`) and throws an internal error (`unhandled_key`) for a field the checker accepted but nothing consumed. The test only exercises the keys it lists, so a new field must be added to it.

`tests/examples/*.tid` are example scripts used by the tests through `TIDE_EXAMPLES_DIR`. Some are expected to fail checking on purpose: `04_fork_mainnet.tid` contains a placeholder address, and `05_scenarios.tid` and `06_local_defi.tid` use the reserved `warp`/`snapshot`/`restore` statements. `07_e2e_tokens.tid` is the script the e2e test runs.

## Checklists

### Add a builtin

1. `src/ast.hpp`: add an enumerator to `BuiltinId`.
2. `src/sema.cpp`: add a row to `builtin_table` (Tide name, id, `pure` or the `Op` it performs), then a `case` in `check_builtin` that checks arity and argument types and returns the result type. A check longer than a line or two goes into a `check_<name>` helper next to `check_builtin`, which keeps the function under the complexity limit.
3. `src/interp.cpp`: add a `case` in `call_builtin`, again delegating anything non-trivial to a `builtin_<name>` helper next to it. If it needs network state, read through the active `RuntimeClient` or `GenesisBuilder`, never a concrete backend.
4. If it needs new Solana logic (an encoding, a layout, a derivation), put it in `src/solana.{hpp,cpp}` and test it in `tests/solana_test.cpp` against SDK-generated vectors.
5. Tests: checker errors in `frontend_test.cpp`, behaviour in `interp_test.cpp`.
6. Document it in the [standard library reference](../reference/standard-library.md).

### Add a genesis field

1. `src/sema.cpp`: add a `FieldSpec` to the entity's array (for example `mint_fields`) with its `Ty` and whether it is required. If it needs a new `Ty`, map it to a type in the checker's `Ty` switch.
2. `src/interp.cpp`: read it in the entity's `put_*` function with `f.get(...)` (optional) or `f.require(...)` (required), applying the default when absent. Without this, `check_all_read` throws an internal error.
3. If the value must reach the backend as a new kind of data, extend the spec in `src/state_builder.hpp` (`AccountSpec`, `ProgramSpec`, ...), then handle it in `src/solana_backend.cpp` and in `FakeBackend` in `tests/interp_test.cpp`. Most entity fields only change the account bytes built with `src/solana.cpp` layouts, and need no boundary change.
4. Add the field to the script in the field-handler consistency test, plus targeted tests.
5. Document it in [genesis](../language/genesis.md).

The same steps apply to `options`, `validator`, `transaction`, `instruction` and `invoke` keys; their interpreter dispatches end in `unhandled_key`.

### Add a backend

1. Create a library target in `CMakeLists.txt` that links `tide_core` (keep its own dependencies out of `tide_core`).
2. Implement `NetworkBackend`, `GenesisBuilder` and `RuntimeClient` from `src/state_builder.hpp`. Honour the lifecycle order, make `abort()` `noexcept` and safe at any point, and report errors as `TideError` with the request's `origin`.
3. Respect `NetworkConfig::confirm_timeout_ms` (falling back to `default_confirm_timeout_ms`) and resolve relative paths against `NetworkConfig::base_dir`.
4. Select it in `src/main.cpp`, where `SolanaTestValidatorBackend` is constructed today.
5. Interpreter tests need no change; they already run against `FakeBackend`. Add a backend-specific test modelled on `tests/e2e_test.cpp`, opt-in if it starts processes.

See also: [concepts](../getting-started/concepts.md), [lifecycle](../guides/lifecycle.md), [solana-test-validator backend](../backends/solana-test-validator.md).
