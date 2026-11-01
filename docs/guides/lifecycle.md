# The `tide up` lifecycle

`tide up` turns a script into a running local network. It does this in a fixed
sequence of phases, and each phase exists because of a constraint of the
underlying validator: some state can only be written before the ledger exists,
some only by sending transactions, and some only after the network is live.
This page walks through each phase, what it writes to disk, what happens when
something fails, and how `tide down` and re-runs behave.

The examples use `tests/examples/07_e2e_tokens.tid` from the repository, saved
as `net.tid` in `/home/you/project`. It declares one network, `e2e`, with
genesis accounts, a bootstrap block and two scenarios.

## Phases at a glance

| # | Phase | Progress line on stdout | Needs a validator | Skipped by `--dry-run` |
|---|-------|-------------------------|-------------------|------------------------|
| 1 | Parse and check | none (diagnostics only) | no | no |
| 2 | Select the network | none | no | no |
| 3 | Global and network bindings | none | no | no |
| 4 | Configuration | `==> configuring network '<name>'` | no | no (but preflight checks are) |
| 5 | Genesis | `==> building genesis state` | no | no |
| 6 | Bootstrap (optional) | `==> running bootstrap` | private instance | yes |
| 7 | Persist | `==> persisting state` | stops the private instance | runs in reduced form |
| 8 | Launch | `==> launching network` | long-running instance | yes |
| 9 | Scenarios | `==> scenario '<name>'` | the launched network | yes |
| 10 | Detach and exit | `network '<name>' is running` | keeps running | yes |

## 1. Parse and check

**Why:** a mistake should be reported before anything is started or written.
Starting a validator takes seconds; a typo should take milliseconds.

`tide up` first does exactly what [`tide check`](../reference/cli.md#tide-check)
does: it parses the file and its imports and runs the semantic checker (names,
types, required fields, which statements are allowed in which block). If this
produces any error, `tide up` prints the diagnostics and exits with code 1.
Nothing is written to disk.

## 2. Select the network

A file may declare several networks; `tide up` runs exactly one. The rules are
in the [CLI reference](../reference/cli.md#network-selection): `--network <name>`
picks one, otherwise the file must declare exactly one network. A selection
error exits with code 2.

## 3. Global and network bindings

Top-level `let` and `keypair` bindings of every module are evaluated (imports
first), then the `let` bindings declared directly inside the selected
`network` block. These values are available to every later phase. An error
here (for example a failing builtin call) exits with code 1 before any
configuration happens. See
[declarations and scope](../language/declarations-and-scope.md).

## 4. Configuration

**Why:** the validator's command line (ports, epoch length, ledger location)
must be known before genesis state can be laid out on disk.

The `options` and `validator` blocks are evaluated into a network
configuration and handed to the backend (see
[networks](../language/networks.md)). The
[solana-test-validator backend](../backends/solana-test-validator.md) then:

1. Rejects configurations it cannot run: more than one `validator` block, or a
   validator `identity`.
2. Computes the state directory, `<state dir>/<network>/`, where `<state dir>`
   is `--state-dir` or `.tide` next to the script.
3. Runs preflight checks:
   - the network must not already be running (see
     [re-running `tide up`](#re-running-tide-up)), also checked by
     `--dry-run`;
   - unless `--dry-run` is given, the validator binary
     (`solana-test-validator` or `--validator`) must be found;
   - unless `--dry-run` is given, the RPC port (default 8899) and faucet port
     (default 9900) must be free.

A preflight failure exits with code 1 and nothing has been started.

## 5. Genesis

**Why:** some state cannot be created by transactions at all, or only with
great effort: accounts owned by arbitrary programs, token accounts with a
given balance, preloaded programs, disabled features. A validator accepts this
state only when it creates a new ledger, so it is built first, while no
validator is running. This is also why genesis cannot send transactions.

Each `genesis` block runs in order. Statements such as `account`, `mint`,
`program`, `clone`, `feature` and `sysvar` add to an in-memory plan of the
initial state (see [genesis](../language/genesis.md)). Queries such as
`balance(...)` read that plan. After the blocks finish, cross-declaration
checks run (for example a token account whose mint is missing, or a mint whose
`supply` does not match the sum of its token accounts, produce warnings).

The progress output ends with a count, for example
`9 genesis account(s) defined`. The count includes programs.

## 6. Bootstrap (optional)

**Why:** some setup is only meaningful as real transactions, for example
creating an account through a program so its internal bookkeeping is
correct, or deploying a program with `deploy`. Bootstrap runs those
transactions before anyone else can see the network, so the network you
connect to later already contains their effects.

If the network has a `bootstrap` block:

1. The genesis plan is written to `accounts/` (see [state layout](#state-directory-layout)).
2. A **private** validator instance is started with `--reset` and every genesis
   flag, creating a fresh ledger in `ledger/`. Tide polls until the validator
   is healthy and past slot 0 (see [timeouts](../backends/solana-test-validator.md#timeouts-and-polling)).
3. The bootstrap block runs against it: `transaction`, `invoke`, `deploy`,
   `airdrop`, `await` and `assert` (see [runtime](../language/runtime.md)).

The instance is private in the sense that its pid is never recorded and it is
always stopped before `tide up` continues. It uses the same ports as the final
network, and Tide never reports it as running. This guarantees that scenarios
and external clients only ever see a network whose bootstrap has completed.

Before the block runs, Tide waits until the instance is healthy and its
`confirmed` slot is at least 1, so the first statement can already be a
transaction to a builtin program.

If there is no `bootstrap` block, no validator is started in this phase.

## 7. Persist

**Why:** the network you use must contain exactly the genesis state plus the
bootstrap effects, and must survive Tide exiting. The bootstrap instance has
to be stopped cleanly so its ledger can be resumed.

With a bootstrap block, persist:

1. Reads the current slot at `confirmed` commitment (the target slot) and
   prints `waiting for bootstrap state to finalize (slot N)`.
2. Polls `getSlot` at `finalized` commitment every 400 ms until it reaches
   the target slot, for at most 90 seconds. Only rooted (finalized) slots are
   guaranteed to be present when a ledger is resumed, so this is what makes
   the bootstrap effects durable.
3. Stops the private instance with SIGINT and waits up to 30 seconds for it to
   flush the ledger, then sends SIGKILL.
4. Writes `manifest.json`.

Without a bootstrap block, persist writes `accounts/` and `manifest.json`;
the ledger is created later, at launch.

### State directory layout

Everything lives under `<state dir>/<network>/`, by default
`.tide/<network>/` next to the script:

| Path | Written by | Contents |
|------|-----------|----------|
| `accounts/` | bootstrap start, or persist | One `<address>.json` file per genesis account. The directory is deleted and recreated on every run. Programs are not here; they are passed as validator flags. |
| `ledger/` | the validator | The validator's ledger. Replaced with a fresh one (`--reset`) on every `tide up`. Moved elsewhere by `options { ledger = "..." }`. |
| `manifest.json` | persist | Summary of the persisted network (below). |
| `validator.log` | each validator start | Combined stdout and stderr of every instance, appended across runs. Each start is preceded by `=== tide: starting <bootstrap\|network> instance ===` and the full command line. |
| `validator.pid` | launch | Process id of the running network. Removed by `tide down`. |
| `keys/` | `deploy` | `deployer.json` (a deterministic fee payer that Tide funds by airdrop) and `<program id>.json` for each deployed program. Only present if the script uses `deploy`. |

Each account file uses the JSON account format that
`solana-test-validator --account-dir` loads. For a 55-byte account owned by
the system program:

```json
{
  "account": {
    "data": [
      "9WB1qEA+WylS/Vy4CarH72O6GGG4a+nOtPd8i1ZkFFkqAAAAAAAAAAIAAAABAAAAYQEAAABiAA==",
      "base64"
    ],
    "executable": false,
    "lamports": 1781760,
    "owner": "11111111111111111111111111111111",
    "rentEpoch": 0,
    "space": 55
  },
  "pubkey": "EzahkKFpyXxBdHq49KypJVavS24VpD6FWE7kqyYVibM7"
}
```

### `manifest.json`

| Key | Type | Meaning |
|-----|------|---------|
| `network` | string | Network name. |
| `rpc_url` | string | URL clients should use, for example `http://127.0.0.1:18899`. A `bind_address` of `0.0.0.0` is reported as `127.0.0.1`. |
| `ledger` | string | Absolute ledger path. |
| `accounts` | number | Number of account files in `accounts/`. |
| `programs` | number | Number of preloaded programs (passed as flags). |
| `bootstrapped` | bool | `true` if a bootstrap instance ran and the ledger already contains genesis plus bootstrap. |
| `genesis_accounts` | array | One `{ "address", "owner", "lamports" }` object per account file. It describes genesis, not the state after bootstrap. |
| `command` | array | Only with `--dry-run`: the validator command line, one argument per element. |

Abridged manifest after a real run of `net.tid`:

```json
{
  "accounts": 9,
  "bootstrapped": true,
  "genesis_accounts": [
    {
      "address": "6QRJxFm6STQ9bn6w7kUGomwcqFUhwAMcXR7L7m4QNTFx",
      "lamports": 2039280,
      "owner": "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
    }
  ],
  "ledger": "/home/you/project/.tide/e2e/ledger",
  "network": "e2e",
  "programs": 0,
  "rpc_url": "http://127.0.0.1:18899"
}
```

## 8. Launch

**Why:** the network must outlive the `tide` process, so it is started as an
independent background process rather than a child that dies with Tide.

1. The validator is started in its own session (detached from the terminal),
   with stdin from `/dev/null` and output appended to `validator.log`.
   - After a bootstrap, it is started **without** `--reset` and without the
     genesis flags, so it resumes the persisted ledger. Genesis flags only take
     effect when a ledger is created, so passing them again would be wrong.
   - Without a bootstrap, it is started with `--reset` and every genesis flag,
     creating the ledger now.
2. Tide polls `getHealth` until it answers `ok` and the `confirmed` slot is at
   least 1, within `startup_timeout` (default 120 s). If the process exits during startup, the error includes
   the last 15 lines of `validator.log`.
3. The pid is written to `validator.pid`.
4. After a bootstrap, Tide also waits until the `confirmed` slot reaches the
   slot persisted in phase 7. A resumed validator answers health checks while
   it is still replaying its ledger; without this wait, scenarios could read
   state from before bootstrap.

The known slot-0 issue from [bootstrap](#6-bootstrap-optional) also applies to
the first scenario transaction when there is no bootstrap block.

## 9. Scenarios

**Why:** scenarios are checks and exercises against the network as clients
will see it. They are deliberately not part of setup: a failing scenario
reports a problem but leaves the network usable for investigation.

Scenarios run in declaration order against the launched network.

- `--no-scenarios` skips all of them.
- `--scenario <name>` (repeatable) runs only the named ones. A name that
  matches no scenario of the selected network is a usage error (exit code 2),
  reported before anything starts.
- An error inside a scenario (a failed `assert`, a runtime error) is reported
  as `scenario '<name>' failed: ...`, the scenario is marked `FAILED`, and the
  next scenario runs. The network is not stopped.

See [runtime](../language/runtime.md) for what a scenario can do.

## 10. Detach and exit

Tide releases the network and prints its connection details, then a scenario
summary if any scenario ran:

```text
==> configuring network 'e2e'
==> building genesis state
    9 genesis account(s) defined
==> running bootstrap
    starting solana-test-validator (bootstrap, rpc http://127.0.0.1:18899)
    transaction succeeded: 2sKXkQKCdPYjkN3C7ayPkAjAgGfbJJnHDvjwzF8yakC8e8X962gycQUJhxtcg62cJcks2ArFvfxgzUQtbSozXh61
    transaction succeeded: 2uq6ma3tZDWdXmFcgU9zzYY4Tn6s3STHvTrw7TCFC7Gi7iWqMD4fpBuAiMuULAyqykks49cKaiE2WkgMGU9ama6C
==> persisting state
    waiting for bootstrap state to finalize (slot 3)
    stopping bootstrap instance
==> launching network
    starting solana-test-validator (network, rpc http://127.0.0.1:18899)
==> scenario 'balances'
    scenario 'balances' passed
==> scenario 'rejected_transfer'
    transaction failed
    scenario 'rejected_transfer' passed

network 'e2e' is running
  rpc:   http://127.0.0.1:18899
  state: /home/you/project/.tide/e2e
  pid:   218042
  stop:  tide down net.tid
scenarios: 2 passed, 0 failed
```

The `transaction failed` line is expected here: `rejected_transfer` asserts
that the transfer is refused. The exit code is 0 when every scenario passed
and 3 when at least one failed; in both cases the network keeps running.

## Failure and teardown

Errors in phases 3 to 8 abort the lifecycle: Tide reports the diagnostic,
tears down what it started, and exits with code 1. Teardown sends SIGTERM and
waits up to 10 seconds before SIGKILL.

| Failure point | Example | Left running | Left on disk |
|---------------|---------|--------------|--------------|
| Parse, check, network selection | typo, unknown `--network` | nothing | nothing new |
| Configuration or preflight | port in use, network already running, validator not found | nothing new (an already running network is untouched) | nothing new |
| Genesis | unsupported sysvar field, clone fetch failed | nothing | nothing new |
| Bootstrap start or body | validator exits on startup, failed `assert` | nothing (bootstrap instance stopped) | `accounts/`, a partial `ledger/`, `validator.log`; no `manifest.json` |
| Persist | bootstrap did not finalize within 90 s | nothing (bootstrap instance stopped) | as above |
| Launch | not ready within `startup_timeout`, replay did not reach the persisted slot | nothing (network stopped, `validator.pid` removed) | `accounts/`, `ledger/`, `manifest.json`, `validator.log` |
| Scenario | failed `assert` | the network (exit code 3) | full state, `validator.pid` |

A bootstrap failure looks like this; the bootstrap instance has already been
stopped when the diagnostic is printed:

```tide
keypair alice = seed("alice");

network dev {
  validator primary { rpc_port = 28899; gossip_port = 28001; faucet_port = 29900; dynamic_port_range = "28002-28040"; }
  genesis {
    account alice.pubkey { lamports = 10 SOL; }
  }
  bootstrap {
    assert balance(alice.pubkey) == 5 SOL, "alice should hold 5 SOL";
  }
}
```

```text
==> configuring network 'dev'
==> building genesis state
    1 genesis account(s) defined
==> running bootstrap
    starting solana-test-validator (bootstrap, rpc http://127.0.0.1:28899)
net.tid:9:5: error: assertion failed: alice should hold 5 SOL
 9 |     assert balance(alice.pubkey) == 5 SOL, "alice should hold 5 SOL";
   |     ^
1 error(s), 0 warning(s)
```

### Interrupting `tide up`

Validators run in their own session, so the terminal's Ctrl-C reaches only the
`tide` process. Tide handles SIGINT, SIGTERM and SIGHUP itself:

- During bootstrap, persist, or launch up to the point where the network is
  ready, it prints `tide: interrupted, stopping solana-test-validator`, stops
  the validator it started (SIGTERM, 10 s grace, then SIGKILL) and exits from
  the signal (exit code 130 for Ctrl-C in most shells). Nothing is left
  running.
- Once the network is launched (during scenarios), the network is already
  recorded in `validator.pid`, so Tide exits and leaves it running, as after a
  failed scenario. `tide down` stops it.

Files on disk are left as they were at the moment of the interruption.

## `tide down`

`tide down <file> [--network N] [--state-dir DIR]` stops a network started by
`tide up`.

1. The file is parsed and checked, and the network is selected, with the same
   rules as `tide up`. Use the same `--state-dir` as for `tide up`.
2. It reads `<state dir>/<network>/validator.pid`. If the file is missing, it
   prints `tide: no running network found in <dir>` and exits with code 1.
3. If the process is alive, it sends SIGINT and waits up to 20 seconds, then
   sends SIGKILL. If the process is already gone it prints
   `validator (pid N) was not running`.
4. It removes `validator.pid` and prints `network '<name>' stopped` (exit 0).

```text
stopping validator (pid 218042)
network 'e2e' stopped
```

`tide down` does not delete the ledger, account files, manifest or log. They
remain available for inspection until the next `tide up` replaces them.

## Re-running `tide up`

**Every `tide up` starts from genesis.** The ledger is always recreated with
`--reset` (by the bootstrap instance, or at launch when there is no
bootstrap), and `accounts/` is rewritten. State created by earlier runs or by
clients is not carried over. This is what makes a network reproducible: the
same script always yields the same initial state.

If the network is still running, `tide up` refuses during configuration,
before anything is written:

```text
==> configuring network 'e2e'
net.tid:20:9: error: network 'e2e' is already running (pid 218042); stop it first with 'tide down'
 20 | network e2e {
    |         ^~~
1 error(s), 0 warning(s)
```

The check uses `validator.pid` in the state directory. A stale pid file whose
process no longer exists does not block a new run. Running the same network
from a different `--state-dir` while the first is still up fails on the port
check instead:

```text
==> configuring network 'e2e'
net.tid:22:13: error: RPC port 18899 is already in use (is another validator running?)
 22 |   validator primary {
    |             ^~~~~~~
1 error(s), 0 warning(s)
```

## `--dry-run`

**Why:** to inspect exactly what Tide would hand to the validator, without
starting one (for example on a machine without the Agave tools, or in CI).

With `--dry-run`, `tide up` runs phases 1 to 5, then a reduced persist, and
stops:

- Most configuration preflight checks are skipped: the validator binary need
  not exist and busy ports are not checked. A network that is already running
  from the same state directory is still an error, because the dry run would
  rewrite its `accounts/` and `manifest.json`.
- Genesis runs fully, including `clone` (which fetches from the source
  cluster) and all genesis diagnostics.
- `accounts/` and `manifest.json` are written. The manifest has
  `"bootstrapped": false` and an extra `command` array.
- No ledger is created, no process is started, and bootstrap and scenarios
  are skipped even if present.
- The printed launch command is the one used for the first validator start
  (with `--reset` and the genesis flags).
- The exit code is 0, or 1 on error.

```sh
tide up net.tid --dry-run
```

```text
==> configuring network 'e2e'
==> building genesis state
    9 genesis account(s) defined
    state written to /home/you/project/.tide/e2e
    launch command: solana-test-validator --ledger /home/you/project/.tide/e2e/ledger --rpc-port 18899 --gossip-port 18001 --faucet-port 19900 --dynamic-port-range 18002-18040 --reset --account-dir /home/you/project/.tide/e2e/accounts --slots-per-epoch 64
==> dry run: state persisted; bootstrap, launch and scenarios skipped
```

To dry-run a script whose network is running, stop it first or pass a
separate `--state-dir`.

See also: [CLI reference](../reference/cli.md),
[solana-test-validator backend](../backends/solana-test-validator.md),
[runtime statements](../language/runtime.md).
