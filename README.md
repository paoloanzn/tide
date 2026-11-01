# Tide

Tide is a language for defining deterministic local Solana networks: genesis
state, programs, SPL tokens, cloned accounts, validator configuration, and
initialization/test scripts. This repository contains the reference
implementation: a C++20 frontend built on [lexy](https://lexy.foonathan.net)
and a direct AST interpreter that drives Agave's `solana-test-validator`.

```text
parse ─▶ AST ─▶ semantic validation ─▶ interpret configuration ─▶ construct genesis
      ─▶ bootstrap (private instance) ─▶ persist ─▶ launch (detached) ─▶ scenarios ─▶ tide exits
```

The network keeps running after `tide` exits.

## Building

Requirements: CMake ≥ 3.25, Ninja, a C++20 compiler (GCC 13 / Clang 18
tested), [vcpkg](https://vcpkg.io), and the Agave CLI tools
(`solana-test-validator`, `solana`) on `PATH` for `tide up`.

```sh
export VCPKG_ROOT=~/vcpkg              # libsodium needs autoconf-archive + libtool
cmake --preset default                 # Debug; `--preset release` for Release
cmake --build build/debug
ctest --preset default                 # unit tests
TIDE_E2E=1 ./build/debug/tide_tests "[e2e]"   # full lifecycle against a real validator (~20 s)
```

Dependencies (from `vcpkg.json`): lexy, libsodium (ed25519), OpenSSL
(sha256, big-number curve checks), cpr (HTTP JSON-RPC), nlohmann-json, Catch2.

## Usage

```sh
tide check net.tid                      # parse + semantic validation, no execution
tide up net.tid                         # run the lifecycle, leave the network running
tide up net.tid --dry-run               # build + persist genesis, print the validator command
tide up net.tid --scenario transfer     # run only some scenarios (repeatable); --no-scenarios to skip
tide down net.tid                       # stop the network started by `tide up`
```

Other options: `--network <name>` (when a file declares several networks),
`--state-dir <dir>` (default `<script dir>/.tide`), `--validator <path>`,
`--solana <path>`.

Exit codes: `0` success, `1` diagnostics or lifecycle failure, `2` usage
error, `3` the network is running but at least one scenario failed.

Diagnostics are reported with file, line, column, the source line and a caret,
plus notes (for example, the previous definition of a duplicate) and "did you
mean" suggestions:

```text
net.tid:15:28: error: unknown field 'lamport' in account; did you mean 'lamports'?
 15 |     account alice.pubkey { lamport = 5 SOL; owner = system_program(); }
    |                            ^~~~~~~
```

## Architecture

| File | Role |
|---|---|
| `src/diag.hpp` | Source manager, positions (`Loc`), diagnostics rendering |
| `src/ast.hpp` | AST; sema decorates nodes in place (symbols, types, builtin ids) |
| `src/parser.{hpp,cpp}` | lexy grammar → AST, syntax diagnostics |
| `src/sema.{hpp,cpp}` | Imports, symbol resolution, scoping, type checking, context rules, declaration validation |
| `src/interp.{hpp,cpp}` | AST interpreter, standard library, borsh/Anchor-IDL codec, lifecycle driver |
| `src/state_builder.hpp` | **Backend boundary**: `NetworkBackend`, `GenesisBuilder`, `RuntimeClient` |
| `src/solana.{hpp,cpp}` | Backend-agnostic Solana primitives (keys, base58, PDA, borsh, SPL layouts, tx serialization) |
| `src/solana_backend.{hpp,cpp}` | `solana-test-validator` implementation of the boundary (process management, JSON-RPC) |
| `src/main.cpp` | CLI |

The interpreter never touches RPC, validator processes, or ledger formats. It
hands the backend Solana-level values (`AccountSpec`, `ProgramSpec`,
`TransactionRequest`, …), each carrying an opaque source `Loc` so backend
errors are still reported at the right line. Another backend (for example a
bank-level simulator or a multi-node cluster) implements the same three
interfaces. `tests/interp_test.cpp` exercises the interpreter through an
in-memory fake backend.

## Language semantics

The grammar is the BNF in the design document. This section is the semantic
specification it calls for.

### Lexical structure

* Comments: `// line` and `/* block */`.
* Reserved keywords (not usable as identifiers): `import as source keypair let
  schema fn network options validator genesis bootstrap scenario if else for in
  while break continue return true false null transaction instruction invoke
  deploy airdrop warp snapshot restore assert await account program mint
  token_account associated_token_account lookup_table sysvar feature fork clone
  delete extension`.
* Contextual keywords, meaningful only where the grammar expects them:
  `using from with at slot to by slots epoch time amount enabled disabled` and
  the units `SOL lamports ms s slots`.
* Record keys, entity fields, member names and object keys may be any word,
  including reserved ones (`mint = …` inside a token account is fine).
* Strings use JSON escapes (`\" \\ \/ \b \f \n \r \t \uXXXX`) and may not span lines.

### Literals and units

| Literal | Type | Value |
|---|---|---|
| `42`, `42 lamports`, `42 slots` | `int` | 42 |
| `1.5` | `float` | 1.5 |
| `1.5 SOL` | `int` | 1 500 000 000 (must be a whole number of lamports) |
| `250 ms`, `2 s`, `1.5 s` | `duration` | milliseconds (must be whole) |

Literals are folded at check time, so overflow and fractional lamports are
compile-time errors.

### Types

| Type | Notes |
|---|---|
| `int` | 128-bit signed, checked arithmetic (overflow and division by zero are errors) |
| `u8 u16 u32 u64 u128 i8 … i128` | refinements of `int`: range-checked on store, on argument passing and when serialized |
| `float` (`f64`), `f32` | `f32` only affects serialization |
| `bool`, `string`, `bytes`, `duration` | |
| `pubkey`, `keypair` | `kp.pubkey : pubkey` |
| `source` | an RPC endpoint (`rpc(url)`); `src.url : string` |
| `range` | `a..b` (exclusive), `a..=b` (inclusive), iterable |
| `[T]`, `[T; N]` | lists; `[T; N]` has a fixed length (checked) |
| `Option<T>` | `null` or a `T`; member access through `null` is a runtime error |
| `{k: T, …}` | object (record) types, from object literals |
| `(T, U)` | tuples, schema fields only (written as list literals) |
| `account_meta` | from `readonly(pk)`, `writable(pk)`, `signer(pk)` |
| schema names | the record type described by the schema |

Implicit conversions: `int` → `float`; `T` and `null` → `Option<T>`; `bytes` ↔
`[u8]`; `pubkey` → `account_meta` (writable, signer if it belongs to a signer).
Everything else must match exactly. Values have copy semantics: assigning a
list or object copies it.

Operators: `+ - * / %` on numbers (`int` op `float` → `float`), `duration ± duration`,
`duration * int`, `duration / int`; `+` also concatenates strings, bytes and
lists. `< <= > >=` on numbers, durations and strings; `== !=` on any comparable
pair (not keypairs). `&& || !` on `bool`, short-circuiting. Ranges cannot be
chained (`1..2..3` is a syntax error).

### Declarations, scoping and evaluation order

* **Declare before use.** A name is visible only after its declaration, in the
  scope that declares it and nested scopes. Re-declaring a name in the same
  scope is an error; shadowing in an inner block is allowed.
* **Top level** (`let`, `keypair`, `source`, `schema`, `fn`, `import`) is
  evaluated once, in order, imports first. Top-level bindings are immutable
  outside top-level code.
* **Functions** see their parameters, themselves (recursion), and the top-level
  declarations that precede them. Parameters are immutable. A function with a
  return type must return on every path. Function bodies are *pure*: they cannot
  read network state. Call depth is limited to 256.
* **Networks**: network-level `let`/`keypair`/`source` are evaluated first, then
  `options`/`validator` (configuration), then `genesis`, `bootstrap` and each
  `scenario`. Locals of one block are not visible in another. At most one
  `options`, `genesis` and `bootstrap` per network; scenario and validator names
  are unique.
* `let` variables are the only mutable bindings. `keypair`/`source` declarations
  are only allowed at top level or directly in a network.
* **Imports**: `import "lib.tid" as lib;` makes the module's top-level
  bindings, functions and schemas available as `lib.name`. Paths are relative to
  the importing file. Import cycles are errors. Networks in imported files are
  checked but cannot be launched.
* Execution is deterministic. `seed("name")` derives the ed25519 seed
  `sha256("tide:seed:" + name)`, and a 10 000 000-step budget stops runaway loops.

### Statement contexts

| Statement | top level / fn | genesis | bootstrap / scenario |
|---|---|---|---|
| `let`, assignment, `if`, `for`, `while`, `assert`, calls | fn bodies | ✓ | ✓ |
| `break`/`continue` (inside loops), `return` (fn only) | ✓ | loops | loops |
| `account program mint token_account associated_token_account lookup_table sysvar feature fork clone delete` | | ✓ | |
| `transaction invoke deploy airdrop await` | | | ✓ |
| `warp snapshot restore` | reserved for a future version: always an error | | |

Builtins that read network state (`balance`, `token_balance`, `account_info`,
`exists`) work in genesis (they read the state being built) and at runtime;
`slot()`/`epoch()` work only at runtime.

### Genesis declarations

All fields are type-checked statically; unknown, duplicate and missing required
fields are errors. Defaults apply to omitted fields.

**`account <pubkey> { … }`**: `lamports: u64` (required), `owner: pubkey` (system
program), `data: bytes` (empty), `executable: bool` (false), `rent_epoch: u64` (0).

**`program <name> { … }`**: `id: pubkey` (required), `binary: string` path
(required, must exist), `loader: pubkey` (`bpf_upgradeable_loader()`),
`upgrade_authority: Option<pubkey>` (none, which makes the program immutable; only for upgradeable loaders),
`idl: string` (Anchor IDL path, used by `invoke` and `anchor("Name", …)`).
Program names live in their own namespace.

**`mint <pubkey> { … }`**: `token_program` (`token_program()`, or
`token_2022_program()`), `decimals: u8` (required), `mint_authority`,
`freeze_authority: Option<pubkey>` (none), `supply: u64` (0), `lamports`
(rent-exempt minimum). Token-2022 extensions: `mint_close_authority
{close_authority}`, `permanent_delegate {delegate}`, `non_transferable {}`,
`metadata_pointer {authority, metadata_address}`, `default_account_state {state}`.

**`token_account <pubkey> { … }`**: `token_program`, `mint` and `owner`
(required), `amount: u64`, `delegate`, `delegated_amount` (≤ amount, needs a
delegate), `state` (`"initialized"` | `"frozen"`), `close_authority`, `lamports`.
Extension: `immutable_owner {}`.

**`associated_token_account <pubkey> { … }`**: `token_program`, `mint`, `owner`
(required), `amount`, `lamports`. The address must equal
`ata(owner, mint, token_program)`. Token-2022 ATAs get `ImmutableOwner` automatically.

**`lookup_table <pubkey> { … }`**: `authority: Option<pubkey>`, `addresses: [pubkey]` (≤ 256),
`deactivation_slot` (u64::MAX), `last_extended_slot`, `lamports`.

**`sysvar <name> { … }`**: `clock`, `rent`, `epoch_schedule` with their fields;
backends decide which they can honour (see below).

**`feature <pubkey> = enabled | disabled;`**, **`fork from <source> [at slot N];`**,
**`clone account|program <pubkey> from <source> [at slot N] [{ overrides }];`**
(overrides use the account fields), **`delete account|program <pubkey>;`**
(only for addresses defined earlier in the same genesis).

Checks before launch, while genesis is interpreted:

* No address may be defined twice (accounts, programs, clones). The error carries a note pointing at the first definition.
* ATA addresses match their derivation.
* Token accounts use the same token program as their mint.
* Extensions require Token-2022.
* Program binaries and IDLs exist.
* Warnings: a mint's supply differs from the sum of its token-account balances in genesis, an account is below the rent-exempt minimum or has 0 lamports, a token account refers to a mint not defined in genesis.

### Runtime statements

* **`transaction [as name] { … }`**: keys `fee_payer: pubkey` (default: first
  signer), `signers: [keypair]`, `compute_unit_limit`, `compute_unit_price`,
  plus one or more `instruction <program> { accounts = [...]; data = bytes; }`.
  Plain pubkeys in `accounts` are writable and are signers when they belong to
  `signers`/the fee payer; use `readonly(pk)`, `writable(pk)`, `signer(pk)` to be
  explicit. The fee payer must be in `signers`. With `as name`, failure does
  not abort: `name : {success, signature, error, logs, slot}`. Without a
  binding, a failed transaction aborts the block, and the error includes the program logs.
* **`invoke <program> with { … };`** builds an Anchor instruction from the
  program's IDL, either the `idl` of the program's genesis declaration or an
  `idl = "path"` key. Keys: `method` (required), `accounts: {name: pubkey}`,
  `args: {name: value}`, `signers`, `fee_payer`, `compute_unit_limit`,
  `remaining_accounts`. Accounts are ordered and flagged from the IDL. Fixed
  addresses and well-known programs fill themselves in. Both pre-0.30 and 0.30+
  IDLs are supported, and names match across camelCase and snake_case. Omitted `Option` arguments encode as `None`.
* **`deploy <keypair> from "<path.so>";`** deploys at runtime via `solana program deploy`.
* **`airdrop <pubkey> amount <lamports>;`**
* **`await <bool expr>;`** re-evaluates until true (polled, `confirm_timeout`, default 60 s).
* **`assert <expr> [, "message"];`** fails the current block.

### Lifecycle and failure semantics

1. Configuration and genesis are interpreted. Nothing starts until both succeed.
2. If there is a `bootstrap` block, a private validator instance starts from the genesis state and runs it. Tide waits until the instance's state is finalized, then stops the instance.
3. The state is persisted under `<state dir>/<network>/`: account JSON files, the ledger, and `manifest.json`.
4. The network is launched as a detached process from the persisted state. Tide waits until it has replayed the ledger.
5. Scenarios run against the launched network. A failing scenario is reported but does not stop the network or the other scenarios.
6. Tide detaches and exits. `tide down` stops the network.

A failure in steps 1–4 tears down anything started and leaves no running network.

### Network configuration

`options`: `slots_per_epoch`, `ticks_per_slot`, `warp_slot`, `compute_unit_limit`,
`limit_ledger_size`, `faucet_sol`, `ledger`, `bind_address`,
`startup_timeout: duration`, `confirm_timeout: duration`.
`validator <name>`: `rpc_port`, `gossip_port`, `faucet_port` (u16),
`dynamic_port_range` (`"8000-8020"`), `bind_address`, `identity: keypair`.

### Standard library

| Function | Result |
|---|---|
| `seed(name)`, `keypair_file(path)` | `keypair` |
| `pubkey(base58)` | `pubkey` (literal strings validated at check time) |
| `pda(program, [seeds])`, `pda_bump(...)` | canonical PDA / its bump; seeds are string, bytes or pubkey |
| `ata(owner, mint [, token_program])` | associated token address |
| `system_program() token_program() token_2022_program() associated_token_program() bpf_loader() bpf_loader_deprecated() bpf_upgradeable_loader() loader_v4() memo_program() compute_budget_program() address_lookup_table_program()` | well-known ids |
| `hex(s) base64(s) base58(s) utf8(s) file(path) sha256(x)` | `bytes` |
| `u8(n) u16_le(n) u32_le(n) u64_le(n) i64_le(n)` | little-endian `bytes` |
| `borsh(Schema, obj)`, `anchor(Schema \| "Name", obj)`, `decode(Schema, bytes)` | schema (de)serialization; `anchor` prefixes the 8-byte account discriminator |
| `system_transfer(n) spl_transfer(n) spl_transfer_checked(n, decimals) spl_mint_to(n) memo(s)` | instruction data |
| `readonly(pk) writable(pk) signer(pk [, writable])` | `account_meta` |
| `rent_exempt(size)`, `sol(n)` | lamports |
| `rpc(url)` | `source` (`"mainnet"`, `"devnet"`, `"testnet"`, `"localhost"` accepted) |
| `len(x) str(x) print(...)` | utilities (`print` writes to stdout) |
| `balance(pk) token_balance(pk) account_info(pk) exists(pk)` | state queries (`account_info` → `Option<{address, lamports, owner, data, executable, rent_epoch}>`) |
| `slot() epoch()` | runtime only |

`schema Name using borsh|anchor { field: type; … }` declares a binary layout.
Schema fields need sized types (`u8`…`i128`, `f32`, `f64`, `bool`, `string`,
`bytes`, `pubkey`, `Option<T>`, `[T]`, `[T; N]`, `(T, U)`, other schemas).
Object literals passed to `borsh`/`anchor` are checked against the schema at
compile time. Optional fields may be omitted.

## The `solana-test-validator` backend

| Tide | Validator |
|---|---|
| genesis accounts, mints, token accounts, lookup tables, clones | JSON files loaded with `--account-dir` |
| `program` with `bpf_upgradeable_loader()` / `bpf_loader()` | `--upgradeable-program` / `--bpf-program` |
| `feature X = disabled` | `--deactivate-feature` |
| `fork from S` | `--url S --clone-feature-set` |
| `sysvar clock { slot }`, `epoch_schedule { slots_per_epoch }` | `--warp-slot`, `--slots-per-epoch` |
| options / validator ports | corresponding flags |

Limitations, all reported as positioned diagnostics before anything starts:

* The backend runs a single validator, so a second `validator` block is an error.
* Clones are fetched over RPC at genesis time and reflect the cluster's current
  state. `at slot` is accepted, but you get a warning that it is ignored.
  `fork` copies the feature set only.
* Sysvar fields other than `clock.slot`, `epoch_schedule.slots_per_epoch` and
  `epoch_schedule.warmup = false` cannot be set.
* `bpf_loader_deprecated()` and `loader_v4()` programs cannot be preloaded, and
  a validator `identity` cannot be chosen.
* Builtin program accounts (system, token, Token-2022, ATA) cannot be overwritten.

## Deviations from the BNF

The parser accepts a few supersets, all re-checked semantically with better
messages:
* one statement grammar for all blocks, with context rules enforced by the checker
* reserved words allowed as record and field keys
* `else if` as sugar for `else { if … }`
* trailing commas in lists, objects and argument lists
* an optional `;` after a transaction `instruction` block

The `account(...)` helper is spelled `account_info(...)` because `account` is a
keyword.
