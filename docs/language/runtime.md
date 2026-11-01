# Runtime statements

Genesis describes the state a network starts from, but some state can only
come from executing programs: an Anchor program initialising its own
accounts, a token transfer whose result you want to check, a program deployed
the normal way. Runtime statements send real transactions to a running
`solana-test-validator`. They are allowed only inside `bootstrap` and
`scenario` blocks, because those are the only phases in which a validator is
running.

This page covers the two runtime blocks, the statements that only work in
them (`transaction`, `invoke`, `deploy`, `airdrop`, `await`), how `assert`
behaves at runtime, and the reserved `warp`, `snapshot` and `restore`
statements. General statements (`let`, `if`, `for`, `while`, function calls)
work here too; see [expressions and statements](expressions-and-statements.md).

## Bootstrap and scenarios

A network can declare at most one `bootstrap` block and any number of named
`scenario` blocks. Both run against a live validator, but at different points
of the [lifecycle](../guides/lifecycle.md) and with different consequences.

### `bootstrap`

`bootstrap` exists to produce state that is part of the network you launch,
when that state is easier to create with transactions than to write by hand in
genesis.

1. Tide starts a private validator instance from the genesis state.
2. It runs the `bootstrap` block against that instance.
3. It waits until the instance's latest slot is finalized, so the ledger
   provably contains the bootstrap effects, then stops the instance.
4. The ledger is persisted, and the network is launched from it.

Any failure in `bootstrap` (a failed `assert`, a failed transaction without a
result binding, a timed-out `await`) aborts the whole `tide up`: the private
instance is stopped, no network is launched, and `tide` exits with code 1.

### `scenario`

A `scenario` is a named check or action run against the launched network after
it is up. Scenarios are for testing the network you built, not for building it.

- Scenarios run in declaration order, each against the same live network, so
  a transaction in one scenario is visible to the scenarios after it.
- A failing scenario is reported as `scenario '<name>' failed: <reason>` and
  does not stop the network or the other scenarios. Tide continues with the
  next scenario.
- At the end, Tide prints `scenarios: N passed, M failed` and exits with code
  3 if any scenario failed (the network keeps running).
- `tide up --scenario <name>` (repeatable) runs only the named scenarios, and
  `--no-scenarios` skips them. A name that matches no scenario of the network
  is a usage error (exit code 2), reported before anything starts. See the [CLI reference](../reference/cli.md).

### How they differ

| | `bootstrap` | `scenario` |
|---|---|---|
| How many per network | at most one | any number, unique names |
| Runs against | a private instance started from genesis | the launched network |
| Effects persisted into the launched network | yes | they happen on the launched network itself |
| On failure | the whole `tide up` aborts, nothing keeps running | reported; other scenarios still run, exit code 3 |
| Skipped by `--dry-run` | yes | yes |

Variables declared in one block are not visible in another block. Top-level
and network-level `let` and `keypair` bindings are visible in both. A
`keypair` cannot be declared inside a runtime block; declare it at top level
or directly in the network.

```tide
keypair bob = seed("bob");

network dev {
  genesis {
    account bob.pubkey { lamports = 1 SOL; }
  }

  bootstrap {
    airdrop bob.pubkey amount 1 SOL;   // becomes part of the persisted state
  }

  scenario balances {
    assert balance(bob.pubkey) == 2 SOL, "bob should hold 2 SOL";
  }
}
```

Genesis gives `bob` 1 SOL, the bootstrap airdrop adds another before the
network is launched, and the scenario checks the result on the launched
network.

## `transaction`

`transaction` sends one Solana transaction built from raw instructions. Use it
for any program whose instruction layout you know: system transfers, SPL Token
instructions, memos, or your own program with hand-encoded data.

```text
transaction [as <name>] {
  <key> = <value>;            // zero or more transaction keys
  instruction <program-id> {  // one or more instructions, run in order
    accounts = [ ... ];
    data = <bytes>;
  }
}
```

### Transaction keys

| Key | Type | Default | Notes |
|---|---|---|---|
| `fee_payer` | `pubkey` | first entry of `signers` | Must have a matching keypair in `signers`. |
| `signers` | `[keypair]` | `[]` | Keypairs that sign the transaction. |
| `compute_unit_limit` | `u32` | not set | Prepends a Compute Budget `SetComputeUnitLimit` instruction. |
| `compute_unit_price` | `u64` | not set | Micro-lamports per compute unit. Prepends a Compute Budget `SetComputeUnitPrice` instruction. |

Unknown and duplicate keys are errors at check time. At least one
`instruction` is required:

```text
notx.tid:4:5: error: transaction has no instructions
 4 |     transaction { signers = [alice]; }
   |     ^
1 error(s), 0 warning(s)
```

### Instructions

Each `instruction <program-id> { ... }` takes a `pubkey` program id and two
optional fields. The `;` after the closing brace is optional.

| Field | Type | Default | Notes |
|---|---|---|---|
| `accounts` | `[account_meta]` | `[]` | Plain pubkeys are accepted and converted (see below). |
| `data` | `bytes` | empty | A `[u8]` list literal is also accepted. |

```tide
transaction as tx {
  fee_payer = alice.pubkey;
  signers = [alice];
  compute_unit_limit = 200000;
  compute_unit_price = 1000;
  instruction system_program() {
    accounts = [signer(alice.pubkey), writable(bob.pubkey)];
    data = system_transfer(1000);
  }
  instruction memo_program() {
    accounts = [];
    data = memo("paid bob");
  }
}
assert tx.success;
```

This sends one transaction containing, in order, the two Compute Budget
instructions, a system transfer of 1000 lamports from `alice` to `bob`, and a
memo. Helpers such as `system_transfer` and `memo` build the instruction data;
see [instruction data helpers](../reference/standard-library.md#instruction-data-helpers).

### Account metas

Every entry of `accounts` becomes an account meta (pubkey, signer flag,
writable flag). How the flags are chosen depends on how you write the entry:

| Entry | Writable | Signer |
|---|---|---|
| plain `pubkey` | yes | yes if it is the fee payer or belongs to a keypair in `signers` |
| `writable(pk)` | yes | yes if it is the fee payer or belongs to a keypair in `signers` |
| `readonly(pk)` | no | yes if it is the fee payer or belongs to a keypair in `signers` |
| `signer(pk)` | yes | yes |
| `signer(pk, false)` | no | yes |

The plain-pubkey rule is a convenience: most instruction accounts in test
scripts are writable, and an account owned by a signer usually needs to sign.
When an instruction requires an account to be read-only, wrap it in
`readonly(...)`. Note that the signer flag is always added when the key belongs
to a signer, even for `readonly(pk)`, so `readonly(pk)` means "not writable",
not "not a signer".

```tide
transaction {
  signers = [alice];
  instruction token_program() {
    accounts = [alice_usd, bob_usd, alice.pubkey];
    data = spl_transfer(250000);
  }
}
```

Here all three accounts are writable, and `alice.pubkey` is also a signer
because `alice` is in `signers` (it is also the fee payer, since no
`fee_payer` is given). That matches what SPL Token `Transfer` expects: source,
destination, owner.

When the transaction is compiled, metas for the same key are merged (a key is
a signer if any instruction marks it as a signer, writable if any marks it
writable), and the fee payer is always the first, writable, signing account.
Every account that ends up as a signer must have a keypair in `signers`;
otherwise the transaction cannot be built and the statement fails with
`cannot build transaction: missing signer for <pubkey>`. Keypairs in `signers`
that the transaction does not use are ignored.

### Fee payer

- If `fee_payer` is set, it is used. Otherwise the first keypair in `signers`
  pays.
- With neither, the statement fails at runtime with
  `transaction needs 'fee_payer' or at least one entry in 'signers'`.
- The fee payer must have a keypair in `signers`; otherwise it fails with
  `fee payer <pubkey> has no matching keypair in 'signers'`. This is checked
  even when the transaction has a result binding.

### Size limit and lookup tables

Tide builds legacy (non-versioned) transactions. The serialized transaction,
including the prepended Compute Budget instructions and all signatures, must
fit in 1232 bytes; otherwise the statement fails with
`transaction is <n> bytes; the maximum is 1232`. Address lookup tables are
not used when building transactions: a `lookup_table` declared in
[genesis](genesis.md) creates the table account, but
`transaction` and `invoke` always list every account key inline.

### Result binding and failure

What happens when the transaction fails depends on whether it has an
`as <name>` binding.

**Without a binding**, a failed transaction fails the enclosing block (which
aborts `tide up` in `bootstrap`, or fails the current scenario). The error is
`transaction failed: <error>`, followed by the program logs when the validator
returned any:

```text
token-net.tid:53:5: error: scenario 'bob_spends_alice' failed: transaction failed: Transaction simulation failed: Error processing Instruction 0: custom program error: 0x4
program logs:
  Program TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA invoke [1]
  Program log: Error: owner does not match
  Program TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA consumed 199 of 200000 compute units
  Program TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA failed: custom program error: 0x4
 53 |     transaction {
    |     ^
```

This output comes from the [tutorial](../getting-started/tutorial.md#step-5-scenarios),
where `bob` tries to move tokens out of an account he does not own.

**With a binding**, a transaction that the validator rejects does not stop the
block. The name is bound to an immutable object you can inspect:

| Field | Type | Meaning |
|---|---|---|
| `success` | `bool` | `true` if the transaction was confirmed without error. |
| `signature` | `string` | Transaction signature (base58). Empty if the RPC node rejected the transaction before it was submitted, for example in preflight simulation. |
| `error` | `string` | Empty on success. Otherwise the RPC error message, or the on-chain error as JSON. |
| `logs` | `[string]` | Program log lines (best effort; may be empty). |
| `slot` | `u64` | Slot the transaction was confirmed in; 0 if it was not confirmed. |

```tide
transaction as bad {
  signers = [bob];
  instruction token_program() {
    accounts = [alice_usd, bob_usd, bob.pubkey];
    data = spl_transfer(1);
  }
}
assert !bad.success, "bob must not move alice's tokens";
print("rejected:", bad.error);
```

The scenario expects the transfer to fail and asserts on it. Failures that
happen before the transaction is sent (missing fee payer keypair, missing
signer, size limit) still fail the block, binding or not.

Transactions are confirmed at `confirmed` commitment. Tide polls the signature
status every 300 ms for up to the network's `confirm_timeout` (default 60 s;
see [network options](networks.md)). A transaction that is not confirmed in
time counts as failed, with the error
`transaction was not confirmed within <n>ms`.

A transaction can be the very first statement of `bootstrap` or of a
scenario. Builtin programs such as SPL Token are not usable while a fresh
validator is still at slot 0, so Tide only hands a validator to runtime code
once its `confirmed` slot is at least 1 (see
[the backend page](../backends/solana-test-validator.md#timeouts-and-polling)).

## `invoke`

Hand-encoding instruction data for an Anchor program means computing the
8-byte discriminator, borsh-encoding every argument and listing accounts in
exactly the right order with the right flags. `invoke` does all of that from
the program's Anchor IDL, so you refer to accounts and arguments by name.

```text
invoke <program-id> with { <key> = <value>; ... };
```

The closing `;` after the block is required.

### Invoke keys

| Key | Type | Default | Notes |
|---|---|---|---|
| `method` | `string` | required | Instruction name in the IDL. |
| `accounts` | object | `{}` | `{ name: pubkey, ... }`; an `account_meta` is accepted, but only its pubkey is used. |
| `args` | object | `{}` | `{ name: value, ... }`, encoded with the IDL argument types. |
| `signers` | `[keypair]` | `[]` | As in `transaction`. |
| `fee_payer` | `pubkey` | first entry of `signers` | As in `transaction`. |
| `compute_unit_limit` | `u32` | not set | As in `transaction`. |
| `remaining_accounts` | `[account_meta]` | `[]` | Appended after the IDL accounts, using the [account meta rules](#account-metas) of `transaction`. |
| `idl` | `string` | the genesis program's `idl` | Path to an IDL JSON file, relative to the root script's directory. |

`compute_unit_price` is not an `invoke` key in v0.1.0; use `transaction` if
you need a priority fee.

```tide
invoke counter.pubkey with {
  method = "setValue";               // matches "set_value" or "setValue" in the IDL
  idl = "./counter.json";            // overrides the genesis program's idl
  accounts = { counter_state: state, authority: admin.pubkey };
  args = { new_value: 9 };
  signers = [payer, admin];
  fee_payer = payer.pubkey;
  compute_unit_limit = 100000;
  remaining_accounts = [readonly(extra)];
};
```

Tide looks up `setValue` in the IDL, writes its discriminator and arguments,
orders `counter_state`, `authority` and any accounts it can fill in itself as
the IDL lists them, appends `extra`, and sends the transaction with `payer`
paying.

### Where the IDL comes from

1. The `idl` key of the `invoke`, if present.
2. Otherwise the `idl` field of the [genesis `program` declaration](genesis.md)
   whose `id` equals the invoked program id.
3. Otherwise the statement fails with
   `no IDL known for program <id>; add 'idl = "...";' to its genesis program declaration or to this invoke`.

Both Anchor IDL formats are supported: the 0.30+ format (explicit
`discriminator` arrays, `writable`/`signer`, `pubkey`) and the older format
(`isMut`/`isSigner`/`isOptional`, `publicKey`, camelCase names).

### Name matching

Names are compared exactly first, then after converting camelCase to
snake_case. This applies to the method, the account names and the argument
names, so a script can use `set_value`, `counter_state` and `new_value` against
a pre-0.30 IDL that spells them `setValue`, `counterState` and `newValue`.

### Instruction data

- **Discriminator:** the instruction's `discriminator` array from the IDL if it
  has one; otherwise the first 8 bytes of
  `sha256("global:" + snake_case(method))`, as older Anchor versions do.
- **Arguments:** encoded in IDL order with the IDL types. Supported types:
  integers `u8`...`i128`, `bool`, `f32`, `f64`, `string`, `bytes`,
  `pubkey`/`publicKey`, `vec`, `option`/`coption`, fixed `array`, and `defined`
  structs and enums without variant data. Tuple structs, enum variants with
  data and generic array lengths are rejected with an error.
- An argument whose IDL type is `option` may be omitted; it is encoded as
  `None`. Any other missing argument fails with
  `missing argument '<name>' for instruction '<method>'`, and an extra one with
  `instruction '<method>' has no argument '<name>'`.

### Accounts

Accounts are emitted in the order the IDL lists them, with the IDL's writable
and signer flags; nested account groups are flattened in place. For each IDL
account, the pubkey is taken from:

1. the matching entry of `accounts`, if given;
2. the IDL's fixed `address`, if the IDL has one;
3. a well-known id for these names: `system_program`, `token_program`
   (SPL Token, not Token-2022), `associated_token_program`, `rent` (the Rent
   sysvar);
4. for an optional account, the program id itself (Anchor's encoding of a
   missing optional account), marked read-only;
5. otherwise the statement fails with `missing account '<name>' for instruction '<method>'`.

Tide does not derive PDA accounts from the IDL's `pda` seeds or resolve
`relations`; pass such accounts explicitly, for example with
[`pda(...)`](../reference/standard-library.md#pda-and-pda_bump). An entry of
`accounts` that matches no IDL account fails with
`instruction '<method>' has no account named '<name>'`.

The signer flag comes from the IDL only: listing an account's keypair in
`signers` does not make an IDL account a signer (unlike plain pubkeys in a
`transaction`), but every IDL signer must have a keypair in `signers`.

### Failure

`invoke` has no result binding. A failed invoke always fails the enclosing
block, with `invoke of '<method>' failed: <error>` followed by program logs.
To inspect a failure instead, build the instruction yourself in a
`transaction as <name>`.

## `deploy`

`deploy` deploys a program binary at runtime through
`solana program deploy`, the way a program reaches a real cluster (upgradeable
loader, program data account, buffer). Use it when you need that exact path;
to have a program present from the first slot, prefer a
[genesis `program`](genesis.md).

```text
deploy <keypair> from "<path to .so>";
```

```tide
deploy counter_v2 from "counter.so";
await exists(counter_v2.pubkey);
```

- The keypair is the program id keypair, so the program address is
  `counter_v2.pubkey`. A `pubkey` is rejected at check time.
- The path is relative to the root script's directory (`~` expands to the
  home directory). A missing file fails with
  `program binary '<path>' does not exist`.
- Tide pays with an internal deployer keypair derived from `seed("tide:deployer")`
  and funds it by airdrop (the rent-exempt minimum for twice the binary size, plus 10 SOL) when its
  balance is too low.
- It runs the `solana` CLI (override with `tide up --solana <path>`) at
  `confirmed` commitment. On failure the statement fails with
  `deploy failed: solana program deploy failed:` followed by the CLI output.

## `airdrop`

`airdrop` funds an account from the validator's faucet. It is the simplest way
to give lamports to an account at runtime.

```text
airdrop <pubkey> amount <lamports>;
```

```tide
airdrop bob.pubkey amount 1 SOL;
```

The amount is a `u64` lamport count, so unit literals such as `1 SOL` or
`500000 lamports` work and a fractional value is a check-time error. Tide
requests the airdrop and waits for confirmation like a transaction. A failure
fails the block with `airdrop to <pubkey> failed: <error>`. The faucet's
balance is controlled by the `faucet_sol` [network option](networks.md).

## `await`

Some conditions become true only after the validator makes progress: a slot
number, an account created by another process, a balance after an
asynchronous effect. `await` re-evaluates a boolean expression until it is
true.

```text
await <bool expression>;
```

```tide
await slot() >= 50;
await exists(counter_v2.pubkey);
```

- The condition is evaluated immediately; if it is false, Tide sleeps 400 ms
  and evaluates it again.
- The timeout is the network's `confirm_timeout` option (default 60 s), the
  same value used to confirm transactions. When the deadline passes, the
  statement fails with `await timed out after <n>ms`.
- The condition can use any expression, typically the
  [state queries](../reference/standard-library.md#state-queries) `slot()`,
  `epoch()`, `balance`, `token_balance`, `account_info` and `exists`, which
  read the validator at `confirmed` commitment.

## `assert`

`assert` works in every block (see
[expressions and statements](expressions-and-statements.md)); at runtime it is
how scenarios express their checks.

```text
assert <bool expression> [, "<message>"];
```

The message must be a string literal. A false condition fails the block with
`assertion failed` or `assertion failed: <message>`. In a scenario this is
reported as, for example,
`scenario 'balances' failed: assertion failed: bob should hold 2 SOL`; in
`bootstrap` it aborts `tide up`.

## Reserved: `warp`, `snapshot`, `restore`

The statements `warp to slot|epoch|time <expr>;`, `warp by slots <expr>;`,
`snapshot "<name>";` and `restore "<name>";` are parsed, but they are reserved for
a future version of Tide. In v0.1.0 the checker always rejects them inside
`bootstrap` and `scenario` (and, like other runtime statements, anywhere
else):

```text
warp.tid:5:5: error: 'warp' is reserved for a future version of Tide: live-network control (warp, snapshot, restore) is not supported yet
 5 |     warp by slots 100;
   |     ^
1 error(s), 0 warning(s)
```

To start a network at a later slot, use the `warp_slot` network option or a
[genesis `sysvar clock`](genesis.md) declaration instead.

## Context errors

Runtime statements outside `bootstrap` and `scenario` are check-time errors.
In genesis the error explains why:

```text
ctx.tid:5:5: error: 'airdrop' is a runtime statement and is only allowed inside bootstrap or scenario (found in genesis); genesis builds state directly, use declarations such as 'account' instead
 5 |     airdrop alice.pubkey amount 1 SOL;
   |     ^
1 error(s), 0 warning(s)
```

Genesis cannot send transactions because no validator is running while it is
built; it writes accounts directly instead. The full table of which statement
is allowed where is in [networks](networks.md).

See also: [standard library](../reference/standard-library.md) for the
builtins used above, [schemas](schemas.md) for encoding instruction data,
and [the lifecycle in depth](../guides/lifecycle.md).
