# Concepts

This page covers the ideas you need before you read the language reference:
what a Tide file describes, how it turns into a running network, and which
guarantees Tide gives you. Each section links to the reference page that
covers the topic in full.

## The problem Tide solves

A local Solana test network is usually put together from a shell script: start
`solana-test-validator` with a list of flags, wait for it, airdrop some SOL,
create a mint with the CLI, create token accounts, transfer tokens, and hope
every developer and CI runner gets the same result. The setup lives in flags
and command order. Addresses change between machines, and when one step fails
halfway you are left with a validator in an unknown state.

Tide replaces that script with a description of the network written as code.
A `.tid` file says which accounts, mints, token accounts and programs exist,
how the validator is configured, which transactions run to bring the network
into its starting state, and which checks should pass against it. The `tide`
tool reads the file, validates it, builds the state, and starts the network.

## One file, one or more networks

A Tide file contains top-level declarations (keypairs, constants, schemas,
functions, imports) and one or more `network` blocks. Each `network` block
describes one network that `tide up` can start.

- The network's name (for example `demo`) identifies it on disk: its state
  lives under `<state dir>/<network name>/`, where the default state dir is
  `.tide` next to the script.
- If a file declares several networks, choose one with `--network <name>`.
- Networks in imported files are checked but cannot be launched.

Running `tide up` twice on the same file does not continue from the previous
run. Each `tide up` rebuilds the network from the script, so the file is the
single source of truth.

## A complete example

```tide
// Top level: names and helpers shared by every network in the file.
keypair alice = seed("alice");   // the same key on every machine, every run
keypair bob = seed("bob");

network demo {
  // Static state: decided before any validator runs.
  options { slots_per_epoch = 64; }
  validator primary { rpc_port = 18899; }

  genesis {
    account alice.pubkey { lamports = 10 SOL; }
    account bob.pubkey { lamports = 1 SOL; }
    assert balance(alice.pubkey) == 10 SOL;   // reads the state being built
  }

  // Dynamic actions: run against a live validator.
  bootstrap {
    airdrop bob.pubkey amount 2 SOL;          // baked into the persisted state
  }

  scenario bob_is_funded {
    assert balance(bob.pubkey) == 3 SOL;      // checked against the running network
  }
}
```

`tide up` on this file builds two funded accounts, runs the airdrop on a
private validator instance, saves the result, starts the network, and runs
the scenario against it. The next sections explain each part.

## Two halves of a network: static state and dynamic actions

A network block has two halves, and the split follows one question: does a
validator need to be running?

| Half | Blocks | When it runs | What it can do |
|---|---|---|---|
| Static state | `options`, `validator`, `genesis` | Before any validator starts | Configure the validator; declare accounts, programs, mints, token accounts, lookup tables, sysvars, features, clones |
| Dynamic actions | `bootstrap`, `scenario` | Against a running validator | Send transactions, `invoke` Anchor methods, `deploy`, `airdrop`, `await` conditions |

**Static state** is written directly into the ledger the validator starts
from. A `genesis` block does not send transactions. It declares what exists at
slot 0, for example `account alice.pubkey { lamports = 10 SOL; }`. This makes
it possible to set up state that no transaction could produce, such as a mint
with an arbitrary supply or an account owned by any program, without calling
those programs.

**Dynamic actions** need a validator to process them, so they are only allowed
where one is running. The two dynamic blocks differ in what happens to their
effects:

- `bootstrap` runs once, on a private validator instance started from the
  genesis state. Its effects become part of the network's starting state.
  Use it for setup that must go through real programs: an initialization
  instruction, an SPL transfer, a runtime `deploy`.
- Each `scenario` runs after the network is launched, against the live
  network. Scenarios are checks and experiments. A failing scenario is
  reported, but it does not stop the network or the other scenarios.

The checker enforces the split. A runtime statement in `genesis` is rejected
before anything runs, and the diagnostic says why:

```text
net.tid:4:5: error: 'airdrop' is a runtime statement and is only allowed inside bootstrap or scenario (found in genesis); genesis builds state directly, use declarations such as 'account' instead
 4 |     airdrop alice.pubkey amount 1 SOL;
   |     ^
1 error(s), 0 warning(s)
```

The other direction is also enforced: `account`, `mint` and the other genesis
declarations are rejected in `bootstrap` and `scenario`. The full table of
which statement is allowed where is in
[networks](../language/networks.md).

## Determinism

The same `.tid` file produces the same network on every machine, every time.
Three rules make this hold.

**Keys come from names.** `seed("alice")` derives an ed25519 keypair from the
string `"alice"`: the seed is `sha256("tide:seed:" + name)`. Every run on every
machine gets the same address for `alice`. Addresses computed from those keys,
such as `ata(...)` and `pda(...)`, are therefore stable too. You can hard-code
an address in a test or a front end and it will keep matching.

Seed-derived keys are public: anyone who knows the name can recompute the
private key. Use them for local networks only.

**Constants are folded at check time.** Number literals with units are
converted to exact integers before anything runs, and constant values are
range-checked against the field they are stored in. `1.5 SOL` becomes
`1500000000` lamports. A value that cannot be represented is a diagnostic,
not a silently rounded number:

```text
net.tid:4:39: error: '1.0000000001' is not a whole number of lamports
 4 |     account alice.pubkey { lamports = 1.0000000001 SOL; }
   |                                       ^~~~~~~~~~~~~~~~
1 error(s), 0 warning(s)
```

Integers are 128-bit with checked arithmetic, so overflow and division by
zero are errors rather than wrap-around. See
[types and values](../language/types-and-values.md).

**Execution has a step budget.** Tide code has loops and functions, so a
script could run forever. A run of the interpreter is limited to 10,000,000
steps. When the limit is reached, the run stops with an error at the
offending statement instead of hanging:

```text
net.tid:4:5: error: execution step limit exceeded (10000000 steps); is there an infinite loop?
 4 |     while true { i = i + 1; }
   |     ^
1 error(s), 0 warning(s)
```

A function can be called only where everything its body does is allowed (a
function that sends transactions cannot run in genesis, for example), and the
call depth is limited to 256. See
[expressions and statements](../language/expressions-and-statements.md).

## Check-time and run-time errors

Tide reports problems in two stages. Knowing which stage catches what tells
you how much `tide check` proves.

`tide check` parses the file and runs semantic validation. It executes
nothing and needs no validator. It catches:

- syntax errors and unknown keywords,
- unknown names, duplicate declarations, use before declaration,
- type errors, unknown or missing fields (with "did you mean" suggestions),
- statements used in the wrong block (the genesis/runtime split above),
- constant values that do not fit (fractional lamports, out-of-range literals),
- malformed literal arguments, such as an invalid base58 string in `pubkey("...")`,
- reserved features: `warp`, `snapshot` and `restore` parse but are always
  rejected in v0.1.0.

`tide up` runs the script, so it also catches errors that depend on computed
values or on a live validator:

- genesis consistency, for example the same address defined twice, or an
  `associated_token_account` whose address does not match
  `ata(owner, mint)`,
- failed `assert` statements,
- arithmetic overflow on computed values, and the step budget,
- failed transactions and `await` timeouts,
- environment problems: a port already in use, a network already running,
  a missing program binary.

Errors in configuration, genesis, bootstrap or launch stop `tide up` and leave
no network running. A failing scenario is different: the network keeps
running and `tide up` exits with code `3`. `tide up --dry-run` builds and
persists the genesis state and prints the validator command without starting
anything, which is a quick way to run the genesis checks. Exit codes and
diagnostic format are in the [CLI reference](../reference/cli.md).

## Backends

A Tide script describes Solana-level state: accounts with lamports, owners
and data; programs; token accounts; transactions. It does not mention
validator flags, ledger directories or RPC calls. A *backend* turns that
description into a running network.

Tide v0.1.0 ships one backend, built on Agave's `solana-test-validator`. It
writes genesis accounts as JSON files loaded with `--account-dir`, maps
`options` and `validator` fields to validator flags, and sends runtime
actions over JSON-RPC. It also has limits that come from the validator, for
example it runs a single validator, so a second `validator` block is rejected
by `tide up`. These limits are reported as positioned diagnostics before
anything starts. See
[the solana-test-validator backend](../backends/solana-test-validator.md).

## The network outlives the tide process

`tide up` is not a long-running supervisor. It performs the lifecycle and
exits:

1. Interpret `options`, `validator` and `genesis`. Nothing starts until all
   three succeed.
2. If there is a `bootstrap` block, run it on a private validator instance,
   wait for its state to finalize, and stop the instance.
3. Persist the state under `<state dir>/<network>/` (accounts, ledger,
   `manifest.json`).
4. Launch the network as a detached process from the persisted state.
5. Run the scenarios against it.
6. Print the RPC URL and exit.

The validator keeps running after `tide` exits, so you can point wallets,
tests and the `solana` CLI at it. `tide up` refuses to start a network that is
already running. `tide down <file>` stops it. The lifecycle, the state
directory layout and teardown are covered in
[lifecycle](../guides/lifecycle.md).

Next: build a network step by step in the [tutorial](tutorial.md).
