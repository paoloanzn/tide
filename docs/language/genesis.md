# Genesis

The `genesis` block describes the state a network starts with: funded
wallets, deployed programs, SPL mints and token accounts, lookup tables,
cloned mainnet accounts, sysvar and feature settings. Building this state
with transactions would need a running validator, fee payers and many
round trips. Genesis instead writes it directly, before any validator
exists, so the network starts already in the state you described.

This page lists every genesis declaration. The block's place in a network
and the statement-context rules are covered in [networks](networks.md).

## How genesis works

- A network has at most one `genesis` block. It runs after configuration and
  before `bootstrap` (see [evaluation order](networks.md#evaluation-order)).
- Declarations run in order, like statements. Genesis is ordinary Tide code:
  you can use `let`, `if`, `for`, `while`, `assert` and function calls to
  compute addresses, amounts and account data.
- State queries (`balance()`, `token_balance()`, `account_info()`,
  `exists()`) work in genesis and read the state declared so far.
- Transactions, airdrops and other runtime statements are not allowed: no
  validator is running yet (see [statement contexts](networks.md#statement-contexts)).

```tide
keypair admin = seed("admin");
keypair usd = seed("usd");

fn holder(i: int) -> pubkey {
  return seed("holder-" + str(i)).pubkey;
}

network airdrop_test {
  let holders = 5;

  genesis {
    mint usd.pubkey { decimals = 6; mint_authority = admin.pubkey; supply = holders * 1000000; }

    for i in 0..holders {
      account holder(i) { lamports = 1 SOL; }
      associated_token_account ata(holder(i), usd.pubkey) {
        mint = usd.pubkey; owner = holder(i); amount = 1000000;
      }
    }

    // Queries read the state built so far.
    assert balance(holder(0)) == 1 SOL;
    assert token_balance(ata(holder(4), usd.pubkey)) == 1000000;
    assert !exists(admin.pubkey);
  }
}
```

This genesis creates a mint, five funded wallets and their token accounts in
a loop, then checks the result with state queries before anything starts.

### Fields

Most declarations take a block of `field = value;` entries. The checker
validates them statically:

- unknown fields are errors, with a "did you mean" suggestion;
- a field given twice is an error;
- a missing required field is an error;
- each value must have the field's type (`u64` values are range-checked).

Omitted optional fields take the defaults listed in each table below. Fields
of type `Option<pubkey>` accept a pubkey or `null`; both omitting the field
and writing `null` mean "no authority".

### When the checks run

`tide check` performs only the static checks: syntax, types, fields and
statement contexts. The checks that need values (duplicate addresses, ATA
derivation, token program consistency, file existence and the warnings) run
while genesis is interpreted, which happens in `tide up`. Use
`tide up --dry-run` to run them without starting a validator: it builds and
persists the genesis state, prints the validator command, and stops. In
particular, program binaries and IDL files do not need to exist for
`tide check`, but they must exist for `tide up`.

## account

Creates an account with explicit lamports, owner and data. Use it for wallets
and for program-owned state accounts whose data you encode yourself.

```tide
account alice.pubkey { lamports = 10 SOL; }

account pda(counter.pubkey, ["config"]) {
  lamports = rent_exempt(40);
  owner = counter.pubkey;
  data = borsh(Note, { author: admin.pubkey, amount: 7 });
}
```

The first declaration funds a system-owned wallet. The second creates a PDA
owned by a program, with data encoded from a [schema](schemas.md).

| Field | Type | Default | Notes |
|---|---|---|---|
| `lamports` | `u64` | required | Use `rent_exempt(size)` for the rent-exempt minimum of `size` data bytes. |
| `owner` | `pubkey` | `system_program()` | |
| `data` | `bytes` | empty | A list of integers in `0..255` also works. |
| `executable` | `bool` | `false` | Writing `executable = true` without a loader as owner gives a warning. |
| `rent_epoch` | `u64` | `0` | |

The address is any `pubkey` expression: a keypair's `.pubkey`, `pda(...)`,
`pubkey("<base58>")`, or a function call.

## program

Preloads an on-chain program from a compiled `.so` file, so it is available
from the first slot without a deploy transaction.

```tide
program counter {
  id = counter.pubkey;
  binary = "target/deploy/counter.so";
  upgrade_authority = admin.pubkey;
  idl = "target/idl/counter.json";
}
```

Unlike the other declarations, `program` takes a name (`counter`) rather
than an address. The name appears in diagnostics; the address is `id`.
Program names must be unique within a genesis block, and they do not clash
with variables (here `counter` is also a keypair).

| Field | Type | Default | Notes |
|---|---|---|---|
| `id` | `pubkey` | required | The program address. |
| `binary` | `string` | required | Path to the `.so` file, relative to the script's directory. Must exist when the network is started. |
| `loader` | `pubkey` | `bpf_upgradeable_loader()` | One of `bpf_loader()`, `bpf_loader_deprecated()`, `bpf_upgradeable_loader()`, `loader_v4()`. |
| `upgrade_authority` | `Option<pubkey>` | none | None makes the program immutable. Only valid with `bpf_upgradeable_loader()` or `loader_v4()`. |
| `idl` | `string` | none | Path to an Anchor IDL JSON file. Must exist and be valid JSON. |

The IDL is what lets `invoke` build Anchor instructions for this program
(see [runtime](runtime.md)) and lets `anchor("Name", obj)` encode its account
types (see [schemas](schemas.md#anchor-idl-account-types)).

Errors from this declaration (the message shows the binary's absolute path):

```text
net.tid:8:53: error: program binary '/path/to/missing.so' does not exist
 8 |     program counter { id = counter.pubkey; binary = "missing.so"; }
   |                                                     ^~~~~~~~~~~~
1 error(s), 0 warning(s)
```

```text
net.tid:8:127: error: upgrade_authority requires an upgradeable loader (bpf_upgradeable_loader() or loader_v4())
 8 |     program counter { id = counter.pubkey; binary = "../target/deploy/counter.so"; loader = bpf_loader(); upgrade_authority = admin.pubkey; }
   |                                                                                                                               ^~~~~
1 error(s), 0 warning(s)
```

The `solana-test-validator` backend can only preload programs that use
`bpf_loader()` or `bpf_upgradeable_loader()`; see
[the backend page](../backends/solana-test-validator.md).

## mint

Creates an SPL Token or Token-2022 mint account with the given authorities
and supply.

```tide
mint usd.pubkey {
  decimals = 6;
  mint_authority = admin.pubkey;
  supply = 1000000000;
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `decimals` | `u8` | required | |
| `token_program` | `pubkey` | `token_program()` | `token_program()` or `token_2022_program()`; anything else is an error. |
| `mint_authority` | `Option<pubkey>` | none | None means no more tokens can be minted. |
| `freeze_authority` | `Option<pubkey>` | none | |
| `supply` | `u64` | `0` | Should equal the sum of the mint's token account balances in genesis (a warning otherwise). |
| `lamports` | `u64` | rent-exempt minimum | Computed from the encoded size, including extensions. |

The account is owned by `token_program` and its data uses the SPL mint
layout.

### Token-2022 mint extensions

With `token_program = token_2022_program()`, a mint can carry extensions,
written as `extension <name> { ... }` entries:

```tide
mint pts.pubkey {
  token_program = token_2022_program();
  decimals = 0;
  mint_authority = admin.pubkey;
  extension mint_close_authority { close_authority = admin.pubkey; }
  extension permanent_delegate { delegate = admin.pubkey; }
  extension non_transferable {}
  extension metadata_pointer { authority = admin.pubkey; metadata_address = pts.pubkey; }
  extension default_account_state { state = "initialized"; }
}
```

| Extension | Fields | Notes |
|---|---|---|
| `mint_close_authority` | `close_authority: Option<pubkey>` | |
| `permanent_delegate` | `delegate: Option<pubkey>` | |
| `non_transferable` | none | |
| `metadata_pointer` | `authority: Option<pubkey>`, `metadata_address: Option<pubkey>` | Stores only the pointer, not the metadata itself. |
| `default_account_state` | `state: string` (required) | `"initialized"` or `"frozen"`. |

Omitted or `null` pubkey fields in an extension are stored as "none" (all
zero bytes). Each extension may appear once. Extensions on an SPL Token
(non-2022) mint are an error:

```text
net.tid:8:47: error: extension 'non_transferable' requires token_program = token_2022_program()
 8 |     mint usd.pubkey { decimals = 6; extension non_transferable {} }
   |                                               ^~~~~~~~~~~~~~~~
1 error(s), 0 warning(s)
```

## token_account

Creates a token account at an arbitrary address, with full control over
delegate, state and close authority. For the usual wallet-owned account, use
[associated_token_account](#associated_token_account) instead.

```tide
token_account vault.pubkey {
  mint = usd.pubkey;
  owner = admin.pubkey;
  amount = 400000000;
  delegate = bob.pubkey;
  delegated_amount = 1000;
  state = "frozen";
  close_authority = admin.pubkey;
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `mint` | `pubkey` | required | |
| `owner` | `pubkey` | required | The token owner (not the account owner, which is the token program). |
| `token_program` | `pubkey` | `token_program()` | Must match the mint's token program. |
| `amount` | `u64` | `0` | |
| `delegate` | `Option<pubkey>` | none | |
| `delegated_amount` | `u64` | `0` | Needs a `delegate` and must not exceed `amount`. |
| `state` | `string` | `"initialized"` | `"initialized"` or `"frozen"`. |
| `close_authority` | `Option<pubkey>` | none | |
| `lamports` | `u64` | rent-exempt minimum | Computed from the encoded size, including extensions. |

Token-2022 token accounts accept one extension, `extension immutable_owner {}`
(no fields), which also requires `token_program = token_2022_program()`.

Errors specific to token accounts:

```text
net.tid:9:83: error: token account state must be "initialized" or "frozen"
 9 |     token_account alice.pubkey { mint = usd.pubkey; owner = alice.pubkey; state = "locked"; }
   |                                                                                   ^~~~~~~~
1 error(s), 0 warning(s)
```

```text
net.tid:9:94: error: delegated_amount is set but the account has no delegate
 9 |     token_account alice.pubkey { mint = usd.pubkey; owner = alice.pubkey; delegated_amount = 5; }
   |                                                                                              ^
1 error(s), 0 warning(s)
```

```text
net.tid:9:129: error: delegated_amount (5) exceeds the account amount (1)
 9 |     token_account alice.pubkey { mint = usd.pubkey; owner = alice.pubkey; amount = 1; delegate = bob.pubkey; delegated_amount = 5; }
   |                                                                                                                                 ^
1 error(s), 0 warning(s)
```

The first is reported by `tide check` when the value is a string literal; the
others are reported when genesis is interpreted.

## associated_token_account

Creates the associated token account (ATA) of an owner for a mint: the
address the Associated Token Account program would derive.

```tide
associated_token_account ata(alice.pubkey, usd.pubkey) {
  mint = usd.pubkey;
  owner = alice.pubkey;
  amount = 600000000;
}

associated_token_account ata(bob.pubkey, pts.pubkey, token_2022_program()) {
  token_program = token_2022_program();
  mint = pts.pubkey;
  owner = bob.pubkey;
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `mint` | `pubkey` | required | |
| `owner` | `pubkey` | required | |
| `token_program` | `pubkey` | `token_program()` | Must match the mint's token program and the one used to derive the address. |
| `amount` | `u64` | `0` | |
| `lamports` | `u64` | rent-exempt minimum | |

ATAs take no `delegate`, `delegated_amount`, `state`, `close_authority` or
extension entries. Token-2022 ATAs automatically get the `ImmutableOwner`
extension, as the ATA program would create them.

The address must equal `ata(owner, mint, token_program)`. Writing the
address as `ata(...)` with the same arguments is the simplest way to satisfy
this. A mismatch is an error that shows the expected address:

```text
net.tid:9:30: error: address 6QRJxFm6STQ9bn6w7kUGomwcqFUhwAMcXR7L7m4QNTFx is not the associated token account of owner HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye for mint CCrDymVfTunhmzpxP8kD9QxQ5DbB8rkd2niZCZz3FEGi (expected 7yirD4ToTcAzwAe9KtA1mcRtN861DRrC8d4suy6aoWRS)
 9 |     associated_token_account ata(bob.pubkey, usd.pubkey) { mint = usd.pubkey; owner = alice.pubkey; }
   |                              ^~~
1 error(s), 0 warning(s)
```

## lookup_table

Creates an address lookup table, owned by the Address Lookup Table program,
for versioned transactions.

```tide
lookup_table alt.pubkey {
  authority = admin.pubkey;
  addresses = [alice.pubkey, bob.pubkey, usd.pubkey];
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `authority` | `Option<pubkey>` | none | None makes the table frozen (no further extension). |
| `addresses` | `[pubkey]` | empty | At most 256 addresses. |
| `deactivation_slot` | `u64` | `18446744073709551615` (u64 max) | The maximum value means "active". |
| `last_extended_slot` | `u64` | `0` | |
| `lamports` | `u64` | rent-exempt minimum | |

## sysvar

Sets fields of the `clock`, `rent` or `epoch_schedule` sysvars:

```tide
sysvar clock { slot = 1000; }
sysvar epoch_schedule { slots_per_epoch = 64; warmup = false; }
```

| Sysvar | Field | Type |
|---|---|---|
| `clock` | `slot` | `u64` |
| | `epoch` | `u64` |
| | `unix_timestamp` | `int` |
| | `epoch_start_timestamp` | `int` |
| | `leader_schedule_epoch` | `u64` |
| `rent` | `lamports_per_byte_year` | `u64` |
| | `exemption_threshold` | number |
| | `burn_percent` | `u8` |
| `epoch_schedule` | `slots_per_epoch` | `u64` |
| | `leader_schedule_slot_offset` | `u64` |
| | `warmup` | `bool` |
| | `first_normal_epoch` | `u64` |
| | `first_normal_slot` | `u64` |

All fields are optional; an omitted field keeps the backend's value. Other
sysvar names are rejected by the checker (`unknown or unsupported sysvar`).

The language accepts every field above, but each backend decides which it can
honour. The `solana-test-validator` backend supports only `clock.slot`
(the starting slot), `epoch_schedule.slots_per_epoch` and
`epoch_schedule.warmup = false`; these take precedence over
`options { warp_slot }` and `options { slots_per_epoch }`. Any other field is
an error when the network is started:

```text
net.tid:8:19: error: sysvar field 'rent.lamports_per_byte_year' cannot be set by the solana-test-validator backend (supported: clock.slot, epoch_schedule.slots_per_epoch, epoch_schedule.warmup = false)
 8 |     sysvar rent { lamports_per_byte_year = 1; }
   |                   ^~~~~~~~~~~~~~~~~~~~~~
1 error(s), 0 warning(s)
```

## feature

Turns a runtime feature gate on or off at genesis:

```tide
feature pubkey("EenyoWx9UMXYKpR8mW5Jmfmy2fRjzUtM7NduYMY8bx33") = disabled;
```

The left side is the feature id (a `pubkey` expression); the right side is
`enabled` or `disabled`. Features not mentioned keep the backend's default
(on `solana-test-validator`, all features are active unless deactivated or
copied from a cluster with `fork`). A later `enabled` cancels an earlier
`disabled` for the same feature.

## fork

Bases the network on a public cluster:

```tide
source mainnet = rpc("mainnet");

network forked {
  genesis {
    fork from mainnet;
  }
}
```

The source is a `source` value from `rpc(url)` or a URL string. Besides full
URLs, the names `"mainnet"` (or `"mainnet-beta"`), `"devnet"`, `"testnet"`
and `"localhost"` are accepted. An optional `at slot N` may follow the
source.

On the `solana-test-validator` backend, `fork` copies the cluster's **feature
set only**, never its accounts, and `at slot` is ignored. Both facts are
reported as warnings:

```text
net.tid:5:5: warning: 'at slot' is ignored: the solana-test-validator backend copies the current feature set of the cluster
 5 |     fork from upstream at slot 5;
   |     ^
net.tid:5:5: warning: fork copies the cluster's feature set only; clone the accounts you need explicitly
 5 |     fork from upstream at slot 5;
   |     ^
```

Use [clone](#clone) for the accounts and programs you need.

## clone

Copies an account or a program from a cluster into genesis:

```tide
clone account pubkey("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v") from mainnet;
clone program pubkey("metaqbxxUerdq28cj1RbAWkYQm3ybzjb6a8bt518x1s") from mainnet;

// Override fields of a cloned account.
clone account pubkey("9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM") from mainnet at slot 300000000 {
  lamports = 20 SOL;
};
```

The syntax is
`clone account|program <pubkey> from <source> [at slot N] [{ overrides }];`.
The source works as for [fork](#fork). Note the `;` after the override block.

- `clone account` fetches the account and stores it in genesis. An optional
  override block replaces fields of the fetched account. It takes the
  [account](#account) fields, all optional: `lamports`, `owner`, `data`,
  `executable`, `rent_epoch`.
- `clone program` fetches an executable account. For programs owned by the
  upgradeable loader, the program data account is cloned too. Overrides are
  not allowed for programs:

```text
net.tid:8:53: error: overrides are only supported when cloning accounts, not programs
 8 |     clone program counter.pubkey from rpc("devnet") { lamports = 1; };
   |                                                     ^
1 error(s), 0 warning(s)
```

Clones are fetched over RPC while genesis is interpreted, so `tide up`
(including `--dry-run`) needs network access to the source. A missing
account, a non-executable `clone program` target or an unreachable endpoint is
an error at the clone's line. On `solana-test-validator` the clone reflects
the cluster's current state; `at slot` is accepted but produces a warning that
it is ignored.

## delete

Removes an account or program declared earlier in the same genesis:

```tide
keypair alice = seed("alice");
keypair counter = seed("counter");
let with_counter = false;

network n {
  genesis {
    program counter { id = counter.pubkey; binary = "target/deploy/counter.so"; }
    account alice.pubkey { lamports = 1 SOL; }
    if !with_counter {
      delete program counter.pubkey;
    }
    delete account alice.pubkey;
    account alice.pubkey { lamports = 5 SOL; }
  }
}
```

`delete account|program <pubkey>;` is useful with conditionals and loops,
for example to drop something a loop declared or to make one genesis serve
several variants. After a delete
the address is free again and can be redeclared. Deleting an address that
genesis has not defined is an error:

```text
net.tid:8:20: error: cannot delete HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye: it is not defined earlier in this genesis
 8 |     delete account alice.pubkey;
   |                    ^~~~~
1 error(s), 0 warning(s)
```

## Checks and warnings

While genesis is interpreted, and once it is complete, Tide checks the state
as a whole. Errors stop `tide up` before anything starts; warnings are
printed and the run continues.

### Errors

**Duplicate addresses.** Every address may be defined once, across accounts,
mints, token accounts, lookup tables, programs and clones. The error notes the
first definition:

```text
net.tid:9:13: error: address HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye is already defined in genesis (as account)
 9 |     account alice.pubkey { lamports = 2 SOL; }
   |             ^~~~~
net.tid:8:13: note: previous definition is here
 8 |     account alice.pubkey { lamports = 1 SOL; }
   |             ^~~~~
1 error(s), 0 warning(s)
```

The same applies across kinds, for example an `account` at a program's `id`
(`already defined in genesis (as program 'counter')`).

**ATA derivation.** An `associated_token_account` address must match
`ata(owner, mint, token_program)`; see [above](#associated_token_account).

**Token program consistency.** A token account or ATA must use the same token
program as its mint, when the mint is defined in genesis:

```text
net.tid:9:19: error: token account HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye uses a different token program than its mint
 9 |     token_account alice.pubkey { mint = usd.pubkey; owner = alice.pubkey; }
   |                   ^~~~~
net.tid:8:10: note: mint is defined here
 8 |     mint usd.pubkey { decimals = 6; token_program = token_2022_program(); }
   |          ^~~
1 error(s), 0 warning(s)
```

**Extensions need Token-2022.** Any `extension` entry requires
`token_program = token_2022_program()`.

**Files must exist.** A program's `binary` and `idl` files must exist, and the
IDL must be valid JSON. Relative paths resolve against the script's directory.

**Builtin programs.** The backend refuses to overwrite the system, SPL Token,
Token-2022 and Associated Token Account program accounts
(`cannot overwrite builtin program account ...`).

### Warnings

**Supply versus balances.** A mint's `supply` should equal the total held by
its token accounts in genesis:

```text
net.tid:8:10: warning: mint CCrDymVfTunhmzpxP8kD9QxQ5DbB8rkd2niZCZz3FEGi has supply 1000 but its token accounts in genesis hold 600
 8 |     mint usd.pubkey { decimals = 6; supply = 1000; }
   |          ^~~
0 error(s), 1 warning(s)
```

**Rent.** An account with 0 lamports does not exist on chain, and one below
the rent-exempt minimum for its data size may be collected:

```text
net.tid:8:13: warning: account HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye holds 1000 lamports, below the rent-exempt minimum of 918720 for 4 bytes
 8 |     account alice.pubkey { lamports = 1000; data = hex("00112233"); }
   |             ^~~~~
net.tid:9:13: warning: account CJHsd5oHaKftDqzDFiHpB3LqN82ftpdoYydM8qbEW6Zm has 0 lamports and will not exist on chain
 9 |     account bob.pubkey { lamports = 0; }
   |             ^~~
0 error(s), 2 warning(s)
```

**Unknown mint.** A token account whose mint is not defined in genesis (for
example, a mint you expect to clone or create later):

```text
net.tid:8:19: warning: token account HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye refers to mint CCrDymVfTunhmzpxP8kD9QxQ5DbB8rkd2niZCZz3FEGi, which is not defined in genesis
 8 |     token_account alice.pubkey { mint = usd.pubkey; owner = alice.pubkey; amount = 5; }
   |                   ^~~~~
0 error(s), 1 warning(s)
```

**Executable system account.** `tide check` warns when an `account` sets
`executable = true` while owned by the system program:

```text
net.tid:8:59: warning: executable account is owned by the system program; executable accounts are normally owned by a loader
 8 |     account alice.pubkey { lamports = 1 SOL; executable = true; }
   |                                                           ^~~~
0 error(s), 1 warning(s)
```

## Backend limitations

The language describes genesis independently of any backend. The
`solana-test-validator` backend has these limits, all reported as positioned
errors or warnings before anything starts:

- programs can only use `bpf_loader()` or `bpf_upgradeable_loader()`;
- only `clock.slot`, `epoch_schedule.slots_per_epoch` and
  `epoch_schedule.warmup = false` can be set through `sysvar`;
- `fork` copies the feature set only, and `at slot` on `fork` and `clone` is
  ignored;
- builtin program accounts cannot be overwritten.

See [the solana-test-validator backend](../backends/solana-test-validator.md)
for how genesis maps to validator flags and files.

Next: [runtime statements](runtime.md) for `bootstrap` and `scenario`, and
[schemas](schemas.md) for encoding account data.
