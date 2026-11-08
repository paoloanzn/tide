# Networks

A Tide script describes one or more local Solana networks. Each `network`
declaration bundles everything needed to produce one running network: its
validator configuration, its initial state, the transactions that prepare it,
and the checks you want to run against it. This page covers the anatomy of a
`network` block, every configuration key, and which statements are allowed in
which block.

## The `network` declaration

A `network` is a top-level declaration with a name and a body of entries:

```tide
keypair alice = seed("alice");
keypair bob = seed("bob");

network dev {
  // Network-level binding: visible to the blocks declared after it.
  let starting_balance = 10 SOL;

  options { slots_per_epoch = 64; confirm_timeout = 30 s; }
  validator main { rpc_port = 8899; }

  genesis {
    account alice.pubkey { lamports = starting_balance; }
  }

  bootstrap {
    airdrop bob.pubkey amount 1 SOL;
  }

  scenario alice_is_funded {
    assert balance(alice.pubkey) == starting_balance;
  }
}

network empty_chain {
  options { ticks_per_slot = 8; }
}
```

`dev` uses every kind of entry. `empty_chain` shows that all entries are
optional: a network with only an `options` block, or even an empty body, is
valid and starts a validator with no extra state.

### Why a file may hold several networks

Several networks in one file can share the same top-level keypairs, schemas
and functions while differing in configuration or state, for example a small
`dev` network and a `forked` network that clones mainnet accounts. Network
names must be unique within a file.

`tide check` validates every network in the file. `tide up` and `tide down`
act on one network: if the file declares exactly one, it is selected
automatically; otherwise you pick it with `--network <name>` (see the
[CLI reference](../reference/cli.md)). Networks in imported files are
checked but cannot be launched.

## Entries

| Entry | How many | Purpose |
|---|---|---|
| `let`, `keypair`, `source` | any | Network-level bindings, shared by the blocks after them |
| `options { ... }` | at most one | Network-wide configuration (epochs, timeouts, ledger, ...) |
| `validator <name> { ... }` | any number, unique names | Per-validator configuration (ports, bind address) |
| `genesis { ... }` | at most one | The initial state, built directly before any validator runs |
| `bootstrap { ... }` | at most one | Transactions run once on a private validator; their effects are persisted |
| `scenario <name> { ... }` | any number, unique names | Scripts run against the launched network |

The `solana-test-validator` backend runs a single validator, so it rejects a
second `validator` block when the network is started (see
[Backend limits](#backend-limits)).

### Uniqueness rules

The checker reports a second `options`, `genesis` or `bootstrap` block, and
a repeated `validator` or `scenario` name, with a note pointing at the first
one:

```tide
network dev {
  options { slots_per_epoch = 64; }
  options { ticks_per_slot = 8; }
  scenario smoke { }
  scenario smoke { }
}
```

```text
net.tid:3:3: error: network 'dev' already has an options block
 3 |   options { ticks_per_slot = 8; }
   |   ^
net.tid:2:3: note: previous definition is here
 2 |   options { slots_per_epoch = 64; }
   |   ^
net.tid:5:12: error: duplicate scenario 'smoke'
 5 |   scenario smoke { }
   |            ^~~~~
net.tid:4:12: note: previous definition is here
 4 |   scenario smoke { }
   |            ^~~~~
2 error(s), 0 warning(s)
```

Validator names and scenario names are separate namespaces, and neither
clashes with variable names.

### Evaluation order

When you run `tide up`, a network's entries are not executed strictly top to
bottom. The phases always run in this order, wherever the blocks appear in
the source:

1. Top-level declarations of the script (and its imports), once.
2. All network-level `let`, `keypair` and `source` bindings, in source order.
3. Configuration: `options` and every `validator` block.
4. `genesis`, followed by the cross-declaration [genesis checks](genesis.md#checks-and-warnings).
5. `bootstrap`, if present, on a private validator instance.
6. The state is persisted and the network is launched.
7. Each `scenario`, in source order (all of them, or the ones selected with
   `--scenario`; none with `--no-scenarios`).

Nothing starts until steps 1 to 4 succeed. The [lifecycle guide](../guides/lifecycle.md)
describes steps 5 to 7, failure handling and teardown in detail.

Name resolution, by contrast, follows source order: a name is visible only
after its declaration (see [declarations and scope](declarations-and-scope.md)).
A network-level `let` written after `genesis` is evaluated before genesis
runs, but genesis still cannot refer to it:

```tide
keypair alice = seed("alice");
network dev {
  genesis {
    account alice.pubkey { lamports = amount; }
  }
  let amount = 5 SOL;
}
```

```text
net.tid:4:39: error: unknown identifier 'amount'
 4 |     account alice.pubkey { lamports = amount; }
   |                                       ^~~~~~
1 error(s), 0 warning(s)
```

### Visibility of locals between blocks

Each `genesis`, `bootstrap` and `scenario` block has its own scope. Bindings
declared inside one block, including `let` variables and
[transaction results](runtime.md), are not visible in any other block. To
share a value, declare it at network level (or top level) before the blocks
that use it.

```tide
keypair alice = seed("alice");
network dev {
  genesis {
    let amount = 5 SOL;
    account alice.pubkey { lamports = amount; }
  }
  scenario check_it {
    assert balance(alice.pubkey) == amount;
  }
}
```

```text
net.tid:8:37: error: unknown identifier 'amount'
 8 |     assert balance(alice.pubkey) == amount;
   |                                     ^~~~~~
1 error(s), 0 warning(s)
```

Network-level bindings are not visible from other networks in the same file.

## `options`

`options` holds network-wide settings. Every key is optional; an omitted key
leaves the backend's default in place. Values are ordinary expressions and
may use top-level and network-level bindings, but not state queries such as
`balance()` (no state exists while configuration is evaluated).

```tide
network configured {
  options {
    slots_per_epoch = 64;
    ticks_per_slot = 16;
    warp_slot = 1000;
    compute_unit_limit = 1400000;
    limit_ledger_size = 50000000;
    faucet_sol = 1000000;
    ledger = "ledgers/configured";
    bind_address = "127.0.0.1";
    startup_timeout = 90 s;
    confirm_timeout = 20 s;
  }
}
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `slots_per_epoch` | `u64` | validator default | Slots per epoch. A `sysvar epoch_schedule { slots_per_epoch }` in genesis takes precedence. |
| `ticks_per_slot` | `u64` | validator default | PoH ticks per slot. |
| `warp_slot` | `u64` | none | Start the ledger at this slot. A `sysvar clock { slot }` in genesis takes precedence. |
| `compute_unit_limit` | `u64` | validator default | Maximum compute units per transaction, network-wide. |
| `limit_ledger_size` | `u64` | validator default | Keep at most this many ledger shreds. |
| `faucet_sol` | number (`int` or `float`) | validator default | SOL held by the built-in faucet that serves `airdrop`. |
| `ledger` | `string` | `<state dir>/<network>/ledger` | Ledger directory. Relative paths resolve against the script's directory. |
| `bind_address` | `string` | validator default | IP address the validator binds to. A `validator { bind_address }` overrides it. Tide reaches the RPC endpoint at this address (at `127.0.0.1` when unset or `0.0.0.0`). |
| `startup_timeout` | `duration` | `120 s` | How long Tide waits for a validator to become ready (healthy and past slot 0) or to replay the persisted ledger. |
| `confirm_timeout` | `duration` | `60 s` | How long a transaction may take to confirm, and how long `await` polls before failing. |

Unknown keys are errors, with a suggestion when a known key is close.
Durations must be written with a unit (`30 s`, `500 ms`):

```tide
network dev {
  options { slot_per_epoch = 64; confirm_timeout = 30; }
}
```

```text
net.tid:2:13: error: unknown field 'slot_per_epoch' in options; did you mean 'slots_per_epoch'?
 2 |   options { slot_per_epoch = 64; confirm_timeout = 30; }
   |             ^~~~~~~~~~~~~~
net.tid:2:52: error: type mismatch in field 'confirm_timeout' of options: expected duration, found int
 2 |   options { slot_per_epoch = 64; confirm_timeout = 30; }
   |                                                    ^~
2 error(s), 0 warning(s)
```

## `validator <name>`

A `validator` block configures one validator process: mostly its network
ports. The name identifies the block in diagnostics.

```tide
network configured {
  validator main {
    rpc_port = 18899;
    gossip_port = 18001;
    faucet_port = 19900;
    dynamic_port_range = "18002-18040";
    bind_address = "127.0.0.1";
  }
}
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `rpc_port` | `u16` | `8899` | JSON-RPC port. Tide talks to the network through it. |
| `gossip_port` | `u16` | validator default | Gossip port. |
| `faucet_port` | `u16` | `9900` | Faucet port used by `airdrop`. |
| `dynamic_port_range` | `string` | validator default | Port range for the validator's other sockets, written `"<low>-<high>"`. |
| `bind_address` | `string` | `options.bind_address` | IP address to bind to; overrides `options { bind_address }`. |
| `identity` | `keypair` | generated | Validator identity keypair. Rejected by the `solana-test-validator` backend. |

Port values are range-checked: a literal above 65535 is a check-time error
(`value out of range for u16`), and `0` is rejected when the network is
started (`rpc_port must be between 1 and 65535`). Before launching, Tide also
verifies that the RPC and faucet ports are free, so a second network on the
same ports fails early instead of half-starting.

Using non-default ports lets a Tide network run next to another local
validator.

### Backend limits

The `solana-test-validator` backend runs exactly one validator and lets it
generate its own identity. Both limits are reported when the network is
configured, before anything starts:

```text
net.tid:4:13: error: the solana-test-validator backend runs a single validator, but network 'cluster' declares 2
 4 |   validator secondary { rpc_port = 8898; gossip_port = 8002; }
   |             ^~~~~~~~~
net.tid:3:13: note: first validator is declared here
 3 |   validator primary { rpc_port = 8899; gossip_port = 8001; }
   |             ^~~~~~~
1 error(s), 0 warning(s)
```

These errors come from the backend, not the checker: `tide check` accepts
such scripts, and `tide up` (including `--dry-run`) rejects them. See
[the solana-test-validator backend](../backends/solana-test-validator.md).

## Statement contexts

Tide uses one statement grammar everywhere, and the checker decides which
statements make sense in which block. The rules follow from what exists at
each point of the lifecycle:

- **Top level** runs before any network exists, so it only holds declarations.
- **Functions** can contain any statement or builtin. What a function's body
  does decides where it can be called: a function that sends transactions can
  only be called in bootstrap and scenarios, one that declares accounts only in
  genesis, and one that touches no network at all anywhere (see
  [what a function can do](declarations-and-scope.md#what-a-function-can-do)).
- **Network configuration** (`options`, `validator`) is a list of
  `key = value;` entries, not statements.
- **Genesis** builds the initial state directly, before any validator exists.
  It declares accounts instead of sending transactions: there is no validator
  to send them to, no fee payer, and no slot. It can read the state built so
  far.
- **Bootstrap and scenarios** talk to a running validator. They change state
  only the way a client would, through transactions, airdrops and deployments,
  so privileged declarations such as `account` are not available there.

| Statement | Top level | `fn` body | `genesis` | `bootstrap` / `scenario` |
|---|---|---|---|---|
| `import`, `schema`, `fn`, `network` | yes | no | no | no |
| `keypair`, `source` | yes (also directly inside a `network`) | no | no | no |
| `let` | yes | yes | yes | yes |
| assignment, `if`, `for`, `while`, `assert`, function calls | no | yes | yes | yes |
| `break`, `continue` | no | inside loops | inside loops | inside loops |
| `return` | no | yes | no | no |
| `account`, `program`, `mint`, `token_account`, `associated_token_account`, `lookup_table`, `sysvar`, `feature`, `fork`, `clone`, `delete` | no | yes (callable only in genesis) | yes | no |
| `transaction`, `invoke`, `deploy`, `airdrop`, `await` | no | yes (callable only in bootstrap and scenarios) | no | yes |
| `warp`, `snapshot`, `restore` | reserved for a future version: always an error | | | |

Builtins follow the same logic:

| Builtins | Allowed in |
|---|---|
| `balance`, `token_balance`, `account_info`, `exists` (read network state) | `genesis` (reads the state being built), `bootstrap`, `scenario` |
| `slot`, `epoch` (need a running network) | `bootstrap`, `scenario` |
| every other builtin | anywhere expressions are allowed |

A function body may use any of them; the function then carries the same
restriction to its callers.

Top-level `let` bindings are read-only everywhere: the top level holds no
assignments, and functions and blocks cannot reassign them. Network-level and
block-level `let` variables can be reassigned (see
[declarations and scope](declarations-and-scope.md)).

### Context errors

Each misplaced statement gets an error naming where it was found:

```tide
keypair alice = seed("alice");
fn fund(pk: pubkey) {
  account pk { lamports = 1 SOL; }
}
network dev {
  options { slots_per_epoch = balance(alice.pubkey); }
  genesis {
    airdrop alice.pubkey amount 1 SOL;
    keypair bob = seed("bob");
    let s = slot();
  }
  scenario s1 {
    fund(alice.pubkey);
    account alice.pubkey { lamports = 1 SOL; }
    warp by slots 10;
  }
}
```

```text
net.tid:6:31: error: 'balance' reads network state and can only be used inside genesis, bootstrap or scenario blocks
 6 |   options { slots_per_epoch = balance(alice.pubkey); }
   |                               ^~~~~~~
net.tid:8:5: error: 'airdrop' is a runtime statement and is only allowed inside bootstrap or scenario (found in genesis); genesis builds state directly, use declarations such as 'account' instead
 8 |     airdrop alice.pubkey amount 1 SOL;
   |     ^
net.tid:9:5: error: 'keypair' declarations are only allowed at top level or directly inside a network
 9 |     keypair bob = seed("bob");
   |     ^
net.tid:10:13: error: 'slot' needs a running network and can only be used inside bootstrap or scenario blocks
 10 |     let s = slot();
    |             ^~~~
net.tid:13:5: error: 'fund' declares genesis state and can only be called inside genesis
 13 |     fund(alice.pubkey);
    |     ^~~~
net.tid:3:3: note: 'account' declares genesis state here
 3 |   account pk { lamports = 1 SOL; }
   |   ^
net.tid:14:5: error: 'account' declarations are only allowed inside genesis (found in bootstrap/scenario)
 14 |     account alice.pubkey { lamports = 1 SOL; }
    |     ^
net.tid:15:5: error: 'warp' is reserved for a future version of Tide: live-network control (warp, snapshot, restore) is not supported yet
 15 |     warp by slots 10;
    |     ^
7 error(s), 0 warning(s)
```

Statements outside any block are syntax errors, because the parser only
accepts declarations at top level and entries directly inside a network:

```tide
network n {
  print("x");
}
```

```text
net.tid:2:3: error: expected a network entry (options, validator, source, keypair, let, genesis, bootstrap or scenario)
 2 |   print("x");
   |   ^
1 error(s), 0 warning(s)
```

Functions help genesis and runtime blocks in two ways: pure functions compute
values (addresses, amounts, encoded data), and functions with network
operations package whole steps, such as funding a wallet or calling a
program, for reuse across blocks and files.

Next: [genesis declarations](genesis.md), then [runtime statements](runtime.md).
