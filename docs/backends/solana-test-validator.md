# solana-test-validator backend

Tide's interpreter does not start validators or speak JSON-RPC itself. It
describes the network in Solana terms (accounts, programs, features,
instructions) and hands that description to a **backend**, which turns it into
a real network. Tide v0.1.0 ships one backend, built on Agave's
`solana-test-validator`. The interface between the two is described in
[architecture](../contributing/architecture.md).

This page explains how each Tide construct maps to the validator, which
timeouts apply, and what the backend cannot express.

## Requirements

| Tool | Used for | How it is found |
|------|----------|-----------------|
| `solana-test-validator` | every `tide up` that is not a dry run | `PATH`, or `--validator <path>` |
| `solana` (CLI) | the `deploy` statement only | `PATH`, or `--solana <path>` |

The validator binary is looked up during configuration, before anything is
written. If it is missing:

```text
net.tid:20:9: error: 'solana-test-validator' was not found in PATH; install the Agave tools or pass --validator
 20 | network e2e {
    |         ^~~
1 error(s), 0 warning(s)
```

`tide up --dry-run` does not need either tool. `solana` is invoked only when
a `deploy` statement runs, so a missing CLI surfaces as a failed deploy.

## How a network is run

The backend runs the lifecycle described in
[the lifecycle guide](../guides/lifecycle.md) with these mechanisms:

1. **Genesis** is written as one JSON file per account to
   `<state dir>/<network>/accounts/` and loaded with `--account-dir` when the
   ledger is created. Programs, features and epoch settings become command
   line flags.
2. **Bootstrap** runs on a validator started with `--reset` and all genesis
   flags. Before it is stopped (SIGINT), Tide waits until the last bootstrap
   slot is finalized, so the resumed ledger contains it.
3. **Launch** starts a detached validator. After a bootstrap it resumes the
   existing ledger without genesis flags and Tide waits until replay reaches
   the persisted slot. Without a bootstrap, the launch itself uses `--reset`
   and the genesis flags.
4. **Runtime statements** in bootstrap and scenarios go through JSON-RPC on
   the validator's RPC port.

The full command line of every start is recorded in
`<state dir>/<network>/validator.log`; `tide up --dry-run` prints the first
one without starting anything.

## Mapping reference

### Always passed

| Flag | Value |
|------|-------|
| `--ledger <dir>` | `<state dir>/<network>/ledger`, or `options { ledger }` |
| `--rpc-port <port>` | `validator { rpc_port }`, default 8899 |
| `--reset` | first start of each `tide up` only (creates a fresh ledger) |
| `--account-dir <dir>` | first start only: `<state dir>/<network>/accounts` |

### Network configuration

| Tide | Validator flag or mechanism | Notes |
|------|----------------------------|-------|
| `validator { rpc_port }` | `--rpc-port` | Also determines the RPC URL Tide uses and reports. |
| `validator { gossip_port }` | `--gossip-port` | Omitted if unset. |
| `validator { faucet_port }` | `--faucet-port` | Omitted if unset; validator default 9900. `airdrop` uses this faucet. |
| `validator { dynamic_port_range }` | `--dynamic-port-range` | String such as `"28002-28040"`. |
| `validator { bind_address }` | `--bind-address` | Takes precedence over `options { bind_address }`. |
| `options { bind_address }` | `--bind-address` | Tide connects to this address, or to `127.0.0.1` when it is `0.0.0.0`. |
| `validator { identity }` | not supported | [Error](#validator-identity). |
| `options { slots_per_epoch }` | `--slots-per-epoch` | Overridden by `sysvar epoch_schedule { slots_per_epoch }`. Genesis-only flag. |
| `options { ticks_per_slot }` | `--ticks-per-slot` | Genesis-only flag. |
| `options { warp_slot }` | `--warp-slot` | Overridden by `sysvar clock { slot }`. Genesis-only flag. |
| `options { compute_unit_limit }` | `--compute-unit-limit` | |
| `options { limit_ledger_size }` | `--limit-ledger-size` | |
| `options { faucet_sol }` | `--faucet-sol` | |
| `options { ledger }` | `--ledger` | A relative path is resolved against the script's directory. |
| `options { startup_timeout }` | Tide's wait for the validator | Default 120 s. See [timeouts](#timeouts-and-polling). |
| `options { confirm_timeout }` | Tide's wait for confirmations | Default 60 s. |

"Genesis-only flag" means the flag is passed only on the start that creates
the ledger. A validator resuming a ledger ignores such flags, so Tide does not
pass them at launch after a bootstrap.

### Genesis

| Tide | Validator flag or mechanism | Notes |
|------|----------------------------|-------|
| `account`, `mint`, `token_account`, `associated_token_account`, `lookup_table` | `accounts/<address>.json`, loaded with `--account-dir` | Tide encodes the data (SPL Token layouts, lookup table layout) itself. |
| `clone account ... from <source>` | fetched by Tide with `getAccountInfo` (commitment `confirmed`) during genesis, written as an account file | Overrides in the `{ ... }` block are applied to the fetched account. The validator's own `--clone` is not used. |
| `clone program ... from <source>` | fetched by Tide, written as account files | Must be executable. For an upgradeable program, its program data account is fetched and written too. |
| `program` with `bpf_upgradeable_loader()` (the default) | `--upgradeable-program <id> <binary> <authority>` | `<authority>` is the `upgrade_authority` address, or `none` when unset. |
| `program` with `bpf_loader()` | `--bpf-program <id> <binary>` | |
| `program` with `bpf_loader_deprecated()` or `loader_v4()` | not supported | [Error](#unsupported-program-loaders). |
| `feature <id> = disabled` | `--deactivate-feature <id>` | |
| `feature <id> = enabled` | no flag | The validator activates all features by default; this only cancels an earlier `disabled` for the same id. |
| `fork from <source>` | `--url <url> --clone-feature-set` | Copies the feature set only; [warnings](#fork-copies-the-feature-set-only). |
| `sysvar clock { slot }` | `--warp-slot` | |
| `sysvar epoch_schedule { slots_per_epoch }` | `--slots-per-epoch` | |
| `sysvar epoch_schedule { warmup = false }` | no flag | Matches the validator's default. `true` is an error. |
| other sysvar fields | not supported | [Error](#unsupported-sysvar-fields). |
| `delete account`, `delete program` | removed from the plan | Nothing is written or passed for it. |

Program binaries are passed by absolute path (relative paths in the script
are resolved against the script's directory); they are not copied into the
state directory. Program `idl` files are used by Tide only.

The URL of a `source` used by `clone` or `fork` may be one of these aliases:

| Alias | URL |
|-------|-----|
| `mainnet`, `mainnet-beta`, `m` | `https://api.mainnet-beta.solana.com` |
| `devnet`, `d` | `https://api.devnet.solana.com` |
| `testnet`, `t` | `https://api.testnet.solana.com` |
| `localhost`, `l` | `http://127.0.0.1:8899` |

An example combining several of these:

```tide
keypair prog = seed("prog");
keypair admin = seed("admin");
keypair old = seed("old");

network dev {
  options { ticks_per_slot = 16; warp_slot = 500; compute_unit_limit = 400000; limit_ledger_size = 10000; faucet_sol = 1000; bind_address = "0.0.0.0"; }
  genesis {
    program app { id = prog.pubkey; binary = "prog.so"; upgrade_authority = admin.pubkey; }
    program plain { id = old.pubkey; binary = "prog.so"; loader = bpf_loader(); }
    feature pubkey("EenyoWx9UMXYKpR8mW5Jmfmy2fRjzUtM7NduYMY8bx33") = disabled;
    sysvar clock { slot = 1000; }
    sysvar epoch_schedule { slots_per_epoch = 128; warmup = false; }
  }
}
```

```text
==> configuring network 'dev'
==> building genesis state
    2 genesis account(s) defined
    state written to /home/you/project/.tide/dev
    launch command: solana-test-validator --ledger /home/you/project/.tide/dev/ledger --rpc-port 8899 --bind-address 0.0.0.0 --compute-unit-limit 400000 --limit-ledger-size 10000 --faucet-sol 1000 --reset --account-dir /home/you/project/.tide/dev/accounts --slots-per-epoch 128 --ticks-per-slot 16 --warp-slot 1000 --deactivate-feature EenyoWx9UMXYKpR8mW5Jmfmy2fRjzUtM7NduYMY8bx33 --upgradeable-program CahcrTRmrRS5oBjWaLMznuANAuZ9dRUjkZnu48mXwtWz /home/you/project/prog.so E47x1E5KFevMHZsesS2TyNnk1wZBRiivHUxfAfiVsV1T --bpf-program LMjXaTiJpEYEZcnwxNrkafU2Mzvv61Q46dpUBL3bSv9 /home/you/project/prog.so
==> dry run: state persisted; bootstrap, launch and scenarios skipped
```

Running `tide up net.tid --dry-run` on it shows that `sysvar clock { slot = 1000; }` replaced
`warp_slot = 500`, and that `warmup = false` produced no flag. The two
programs count as genesis accounts in the progress line but are flags, not
account files.

### Runtime statements

| Tide | RPC method or command | Notes |
|------|-----------------------|-------|
| `transaction`, `invoke` | `getLatestBlockhash`, then `sendTransaction` (base64, preflight commitment `confirmed`) | `compute_unit_limit` / `compute_unit_price` become Compute Budget instructions placed first. Transactions over 1232 bytes are rejected before sending. |
| confirmation | `getSignatureStatuses` every 300 ms until `confirmed` or `finalized` | Fails after `confirm_timeout`. Logs are then fetched with `getTransaction` (best effort). |
| `airdrop` | `requestAirdrop` (commitment `confirmed`), then confirmation | Served by the validator's faucet. |
| `deploy` | `solana program deploy --url <rpc> --keypair keys/deployer.json --program-id keys/<id>.json --commitment confirmed <binary>` | The deployer keypair is deterministic. Before deploying, Tide airdrops it enough to cover twice the binary's rent plus 10 SOL if its balance is lower. |
| `account_info`, `balance` and other state queries | `getAccountInfo` (base64, commitment `confirmed`) | |
| `slot()` | `getSlot` (commitment `confirmed`) | |
| `epoch()` | `getEpochInfo` (commitment `confirmed`) | |

A freshly started validator answers health checks while still at slot 0,
when builtin programs are not yet usable (a transaction then fails with
"Program is not deployed"). Tide therefore treats a validator as ready only
when it is healthy and its `confirmed` slot is at least 1; see the startup row
below.

## Timeouts and polling

| What | Value | Configurable |
|------|-------|--------------|
| Validator startup (healthy and `confirmed` slot ≥ 1) | 120 s | `options { startup_timeout }` |
| Readiness probe | `getHealth` (2 s request timeout), then `getSlot` (`confirmed`), every 500 ms | no |
| Replay after relaunch (`confirmed` slot reaches the persisted slot) | same as startup timeout, polled every 400 ms | `options { startup_timeout }` |
| Bootstrap finalization before shutdown | 90 s, `getSlot` (`finalized`) every 400 ms | no |
| Transaction confirmation | 60 s, polled every 300 ms | `options { confirm_timeout }` |
| Any other RPC request | 30 s | no |
| Stopping the bootstrap instance | SIGINT, 30 s grace, then SIGKILL | no |
| `tide down` | SIGINT, 20 s grace, then SIGKILL | no |
| Teardown after an error | SIGTERM, 10 s grace, then SIGKILL | no |
| Teardown after SIGINT, SIGTERM or SIGHUP to `tide` (before the network is ready) | SIGTERM, 10 s grace, then SIGKILL | no |

If the validator exits during startup, the error includes the last 15 lines
of `validator.log`. If it does not become ready in time, Tide stops it and
names the log file.

## Port checks

Before starting anything, Tide checks that the RPC port (default 8899) and the
faucet port (default 9900) can be bound. Gossip and dynamic ports are not
checked; a conflict there shows up as the validator exiting during startup.

```text
net.tid:22:13: error: RPC port 18899 is already in use (is another validator running?)
 22 |   validator primary {
    |             ^~~~~~~
```

To run several networks at once, give each one its own `rpc_port`,
`gossip_port`, `faucet_port` and `dynamic_port_range`.

## Limitations

Each limitation below is reported when `tide up` reaches the construct
(`tide check` accepts these scripts). `tide up --dry-run` reports them too,
since configuration and genesis run in a dry run. Errors stop the run with
exit code 1; warnings do not.

### Single validator

`solana-test-validator` runs one node, so a network may declare at most one
`validator` block.

```tide
network cluster {
  validator primary { rpc_port = 28899; }
  validator secondary { rpc_port = 28898; }
}
```

```text
net.tid:3:13: error: the solana-test-validator backend runs a single validator, but network 'cluster' declares 2
 3 |   validator secondary { rpc_port = 28898; }
   |             ^~~~~~~~~
net.tid:2:13: note: first validator is declared here
 2 |   validator primary { rpc_port = 28899; }
   |             ^~~~~~~
```

### Validator identity

The validator generates its own identity keypair; Tide cannot pass one.

```tide
keypair node = seed("node");

network dev {
  validator primary { identity = node; }
}
```

```text
net.tid:4:13: error: validator 'identity' cannot be set with the solana-test-validator backend (it generates its own identity)
 4 |   validator primary { identity = node; }
   |             ^~~~~~~
```

### Clone at a slot

Tide fetches clones with `getAccountInfo`, which returns current state. A
historical slot cannot be requested, so `at slot` is ignored with a warning
and the clone proceeds:

```tide
keypair alice = seed("alice");
source local = rpc("http://127.0.0.1:1");

network dev {
  genesis {
    clone account alice.pubkey from local at slot 100;
  }
}
```

```text
net.tid:6:5: warning: 'at slot' is ignored: the solana-test-validator backend clones the current state of http://127.0.0.1:1
 6 |     clone account alice.pubkey from local at slot 100;
   |     ^
net.tid:6:5: error: RPC request 'getAccountInfo' to http://127.0.0.1:1 failed: Failed to connect to 127.0.0.1:1 after 0 ms: Could not connect to server
 6 |     clone account alice.pubkey from local at slot 100;
   |     ^
```

(The source here is unreachable on purpose, which also shows the error for a
failed fetch.) Other clone errors:

| Situation | Message |
|-----------|---------|
| account missing on the source | `account <address> does not exist on <url>` |
| `clone program` of a non-executable account | `<address> on <url> is not an executable program account` |
| upgradeable program whose program data is missing | `program data account <address> is missing on <url>` |
| RPC error from the source | `fetching <address> from <url> failed: <message>` |
| cloned account already in the plan (for example a program data account added by an earlier `clone program`) | `account <address> is already part of the genesis state`, with a note `it was added here` |

### Fork copies the feature set only

`fork from <source>` maps to `--clone-feature-set`, which copies which
features are active on the cluster. It does not copy accounts. Tide always
warns about this, and additionally warns when `at slot` is given:

```tide
source upstream = rpc("https://api.mainnet-beta.solana.com");

network forked {
  genesis {
    fork from upstream at slot 300000000;
  }
}
```

```text
net.tid:5:5: warning: 'at slot' is ignored: the solana-test-validator backend copies the current feature set of the cluster
 5 |     fork from upstream at slot 300000000;
   |     ^
net.tid:5:5: warning: fork copies the cluster's feature set only; clone the accounts you need explicitly
 5 |     fork from upstream at slot 300000000;
   |     ^
0 error(s), 2 warning(s)
```

Use `clone account` and `clone program` for the accounts the network needs.

### Unsupported sysvar fields

The checker accepts every field of `clock`, `rent` and `epoch_schedule`, but
the validator exposes flags for only a few. Supported: `clock.slot`,
`epoch_schedule.slots_per_epoch` and `epoch_schedule.warmup = false`. Any
other field is an error at the field:

```tide
network dev {
  genesis {
    sysvar clock { slot = 1000; unix_timestamp = 1700000000; }
  }
}
```

```text
net.tid:3:33: error: sysvar field 'clock.unix_timestamp' cannot be set by the solana-test-validator backend (supported: clock.slot, epoch_schedule.slots_per_epoch, epoch_schedule.warmup = false)
 3 |     sysvar clock { slot = 1000; unix_timestamp = 1700000000; }
   |                                 ^~~~~~~~~~~~~~
```

Enabling epoch warmup has its own message:

```tide
network dev {
  genesis {
    sysvar epoch_schedule { slots_per_epoch = 32; warmup = true; }
  }
}
```

```text
net.tid:3:51: error: epoch warmup cannot be enabled with the solana-test-validator backend
 3 |     sysvar epoch_schedule { slots_per_epoch = 32; warmup = true; }
   |                                                   ^~~~~~
```

### Unsupported program loaders

The validator can preload programs only for the BPF loader
(`--bpf-program`) and the upgradeable BPF loader (`--upgradeable-program`).
`bpf_loader_deprecated()` and `loader_v4()` are accepted by the checker but
rejected here:

```tide
keypair prog = seed("prog");

network dev {
  genesis {
    program legacy { id = prog.pubkey; binary = "prog.so"; loader = bpf_loader_deprecated(); }
  }
}
```

```text
net.tid:5:5: error: program 'legacy': the solana-test-validator backend can only preload programs with bpf_loader() or bpf_upgradeable_loader()
 5 |     program legacy { id = prog.pubkey; binary = "prog.so"; loader = bpf_loader_deprecated(); }
   |     ^
```

### Builtin program accounts

The validator provides the System, SPL Token, Token-2022 and Associated Token
programs itself. Tide refuses to write a genesis account at any of these
addresses, whether declared or cloned:

```tide
network dev {
  genesis {
    account token_program() { lamports = 1 SOL; }
  }
}
```

```text
net.tid:3:13: error: cannot overwrite builtin program account TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA
 3 |     account token_program() { lamports = 1 SOL; }
   |             ^~~~~~~~~~~~~
```

### Other constraints

- State is not resumed across runs: every `tide up` recreates the ledger with
  `--reset`. See [re-running `tide up`](../guides/lifecycle.md#re-running-tide-up).
- `warp`, `snapshot` and `restore` are reserved for a future version and
  rejected by the checker, independently of the backend.

See also: [the lifecycle guide](../guides/lifecycle.md),
[CLI reference](../reference/cli.md), [genesis](../language/genesis.md).
