# Standard library

Tide has no import-able standard library modules. Instead, the checker knows a
fixed set of builtin functions, available everywhere without a declaration.
This page lists every builtin in Tide v0.1.0, grouped by purpose, with its
signature, result, errors and where it may be used.

## Reading the signatures

Signatures are written as `name(param: type, ...) -> result`.

- Types are the ones described in [types and values](../language/types-and-values.md).
  `int` is the unrefined 128-bit integer; `u8`, `u16`, `u32`, `u64`, `i64` are
  refinements, so an argument of that type is range-checked (at check time for
  literals, at run time otherwise).
- `[T]` is a list of `T`; `Option<T>` is `T` or `null`.
- `[p: T]` marks an optional trailing parameter.
- `...` means any number of arguments of any type.
- `-> void` means the call produces no value and cannot be used as one.
- `Schema` stands for the name of a [schema](../language/schemas.md)
  declaration (possibly module-qualified, such as `lib.Config`).

A builtin must be called. Writing its name without parentheses is an error:

```text
notcalled.tid:1:9: error: builtin function 'system_program' must be called, e.g. 'system_program(...)'
 1 | let x = system_program;
   |         ^~~~~~~~~~~~~~
1 error(s), 0 warning(s)
```

Calling a builtin with the wrong number of arguments is reported as, for
example, `'ata' expects 2 to 3 argument(s), found 1`, and a wrong argument
type as `type mismatch in argument 1 of 'sha256': expected string or bytes, found int`.

### Where builtins may be used

Most builtins are pure: they compute a value from their arguments and work in
every context (top level, functions, network configuration, genesis,
bootstrap and scenarios). Two groups are restricted because they read the
state of a network:

| Group | Builtins | Allowed in | Why |
|---|---|---|---|
| State queries | `balance`, `token_balance`, `account_info`, `exists` | `genesis`, `bootstrap`, `scenario` | They need network state. In genesis they read the state built so far; at runtime they read the validator. |
| Runtime only | `slot`, `epoch` | `bootstrap`, `scenario` | They need a running validator; genesis has no slot yet. |

Neither group may be used in a function body, at top level, or in `options` and
`validator` blocks, even if the function is only called from a scenario.
Functions stay pure so that their result does not depend on where they are
called. Pass the queried value in as an argument instead.

```text
fnstate.tid:2:10: error: 'balance' reads network state and can only be used inside genesis, bootstrap or scenario blocks
 2 |   return balance(k) > 0;
   |          ^~~~~~~
1 error(s), 0 warning(s)
```

```text
slot.tid:3:13: error: 'slot' needs a running network and can only be used inside bootstrap or scenario blocks
 3 |     let s = slot();
   |             ^~~~
1 error(s), 0 warning(s)
```

`print`, `file` and `keypair_file` are allowed in functions. `tide check` does
not execute anything, so file-reading builtins only report missing files
during `tide up`.

Relative paths given to `file`, `keypair_file` (and to `deploy` and IDL paths)
are resolved against the directory of the root script passed to `tide`, even
when the call is in an imported file. A leading `~` expands to the home
directory.

## Keys and addresses

| Signature | Result | Notes |
|---|---|---|
| `seed(name: string) -> keypair` | deterministic keypair | ed25519 keypair whose secret seed is `sha256("tide:seed:" + name)`. The same name gives the same key on every machine. |
| `keypair_file(path: string) -> keypair` | keypair from a file | Reads a Solana CLI keypair file (JSON array of 64 bytes). Fails at run time if the file cannot be read, is not a JSON byte array, does not hold 64 bytes, or its public half does not match its secret half. |
| `pubkey(text: string) -> pubkey` | parsed address | `text` must be base58 for 32 bytes. A string literal is validated at check time; a computed string fails at run time with `invalid pubkey '<text>'`. |
| `pda(program: pubkey, seeds: [seed]) -> pubkey` | canonical program-derived address | See [pda and pda_bump](#pda-and-pda_bump). |
| `pda_bump(program: pubkey, seeds: [seed]) -> u8` | canonical bump | The bump that `pda` used for the same arguments. |
| `ata(owner: pubkey, mint: pubkey [, token_program: pubkey]) -> pubkey` | associated token account address | See [ata](#ata). |

A `keypair` value exposes its address as `.pubkey`. Literal validation of
`pubkey`:

```text
pk.tid:1:23: error: invalid pubkey: expected a base58 string encoding 32 bytes
 1 | let treasury = pubkey("not-a-key");
   |                       ^~~~~~~~~~~
1 error(s), 0 warning(s)
```

### pda and pda_bump

Program-derived addresses are how Solana programs own state at predictable
addresses. `pda` finds the canonical address the same way
`Pubkey::find_program_address` does: it tries bumps from 255 downwards and
returns the first address that is off the ed25519 curve.

- Each seed is a `string` (its UTF-8 bytes), `bytes` or `pubkey` (its 32
  bytes). Integers are not seeds; encode them explicitly with `u8(n)`,
  `u64_le(n)` and so on, so the byte layout matches the program.
- At most 15 seeds (the bump is the 16th), each at most 32 bytes.
- With a list literal these limits are checked at check time; with a computed
  list, at run time.

```tide
let config = pda(vault_program.pubkey, ["config"]);
let vault = pda(vault_program.pubkey, ["vault", alice.pubkey, u64_le(7)]);
let vault_bump = pda_bump(vault_program.pubkey, ["vault", alice.pubkey, u64_le(7)]);
```

The second address mixes a string, a pubkey and an 8-byte little-endian
integer as seeds. `vault_bump` is the bump byte a program would store or
re-derive for `vault`.

```text
seedint.tid:1:41: error: seeds must be string, bytes or pubkey, found int (use u8(n) or u64_le(n) for integers)
 1 | let p = pda(system_program(), ["vault", 7]);
   |                                         ^
1 error(s), 0 warning(s)
```

Other seed errors: `seed is <n> bytes; the maximum is 32` and
`too many seeds (16); the maximum is 15 plus the bump`.

### ata

`ata(owner, mint)` returns the associated token account address for `owner`
and `mint` under SPL Token: the PDA of the Associated Token Account program
with seeds `[owner, token_program, mint]`. For Token-2022 mints, pass the
token program as the third argument, because the token program is part of the
derivation.

```tide
let alice_usd = ata(alice.pubkey, usd.pubkey);
let alice_usd22 = ata(alice.pubkey, usd22.pubkey, token_2022_program());
```

The two addresses differ even for the same owner, because the token program is
one of the seeds. Genesis checks that an `associated_token_account`
declaration's address equals this derivation; see [genesis](../language/genesis.md).

## Well-known program ids

Each takes no arguments and returns a `pubkey`.

| Builtin | Address |
|---|---|
| `system_program()` | `11111111111111111111111111111111` |
| `token_program()` | `TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA` |
| `token_2022_program()` | `TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb` |
| `associated_token_program()` | `ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL` |
| `bpf_loader()` | `BPFLoader2111111111111111111111111111111111` |
| `bpf_loader_deprecated()` | `BPFLoader1111111111111111111111111111111111` |
| `bpf_upgradeable_loader()` | `BPFLoaderUpgradeab1e11111111111111111111111` |
| `loader_v4()` | `LoaderV411111111111111111111111111111111111` |
| `memo_program()` | `MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr` |
| `compute_budget_program()` | `ComputeBudget111111111111111111111111111111` |
| `address_lookup_table_program()` | `AddressLookupTab1e1111111111111111111111111` |

## Bytes and encodings

| Signature | Result | Notes |
|---|---|---|
| `hex(text: string) -> bytes` | decoded hex | Needs an even number of hex digits; `hex("")` is empty bytes. Literals are validated at check time. |
| `base64(text: string) -> bytes` | decoded base64 | Literals validated at check time (`invalid base64 string`). |
| `base58(text: string) -> bytes` | decoded base58 | Any length. Literals validated at check time (`invalid base58 string`). |
| `utf8(text: string) -> bytes` | UTF-8 bytes of `text` | |
| `sha256(data: string or bytes) -> bytes` | 32-byte SHA-256 digest | A string is hashed as its UTF-8 bytes. |
| `file(path: string) -> bytes` | file contents | Read at run time; fails with `cannot read file '<path>'`. |
| `u8(n: u8) -> bytes` | 1 byte | |
| `u16_le(n: u16) -> bytes` | 2 bytes, little-endian | |
| `u32_le(n: u32) -> bytes` | 4 bytes, little-endian | |
| `u64_le(n: u64) -> bytes` | 8 bytes, little-endian | |
| `i64_le(n: i64) -> bytes` | 8 bytes, little-endian two's complement | |
| `borsh(Schema, value: object) -> bytes` | borsh encoding | See [schema encoding](#borsh-anchor-and-decode). |
| `anchor(Schema or name: string, value: object) -> bytes` | Anchor account data | 8-byte account discriminator, then borsh. |
| `decode(Schema, data: bytes) -> Schema` | decoded object | Inverse of `borsh`. |

`bytes` values support `+` for concatenation, so instruction data can be
assembled from these pieces, for example `u8(1) + u64_le(500)`. An argument
out of range for the parameter type is an error, for example
`value out of range for u16 in argument 1 of 'u16_le'`. Note that `u8(n)`
produces one byte of `bytes`; it is not a conversion to the `u8` integer type.

```text
hexbad.tid:1:13: error: invalid hex string: expected an even number of hex digits
 1 | let b = hex("abc");
   |             ^~~~~
1 error(s), 0 warning(s)
```

### borsh, anchor and decode

These builtins turn objects into account data and back, using a
[schema](../language/schemas.md) as the layout. They are summarised here; the
schema page describes the encoding of each field type.

- `borsh(Schema, value)` encodes `value` with the schema's field order. An
  object literal is checked against the schema at check time (field names and
  types, integer ranges). `Option` fields may be omitted and encode as `None`.
  If the schema is declared `using anchor`, the 8-byte Anchor account
  discriminator is prefixed, exactly as `anchor(Schema, value)` does.
- `anchor(Schema, value)` writes the Anchor account discriminator
  (`sha256("account:" + Name)`, first 8 bytes) followed by the borsh encoding.
  The first argument can also be a string naming an account type:
  - if a schema with that name is declared (in this file or an imported one),
    that schema is used;
  - otherwise Tide searches the IDLs of `program` declarations made earlier in
    the same genesis for an account of that name, and uses the IDL's
    `discriminator` (or the derived one for older IDLs) and its layout;
  - otherwise it fails at run time with
    `unknown anchor account type '<name>': declare 'schema <name> using anchor { ... }' or give the owning program an 'idl' in genesis`.
- `decode(Schema, data)` decodes `data` into an object of the schema's type, so
  its fields can be read with `.field`. The first argument must be a schema
  name (a string is rejected). For a schema declared `using anchor`, the data
  must start with the matching discriminator. Errors:
  `cannot decode data as '<Schema>': <reason>` and
  `data does not start with the anchor discriminator of '<Schema>'`.

```tide
keypair alice = seed("alice");
keypair vault_program = seed("vault_program");
let config = pda(vault_program.pubkey, ["config"]);

schema Config using borsh {
  admin: pubkey;
  fee_bps: u16;
  label: Option<string>;
}

network dev {
  genesis {
    account config {
      lamports = rent_exempt(32 + 2 + 1);
      owner = vault_program.pubkey;
      data = borsh(Config, { admin: alice.pubkey, fee_bps: 25 });
    }
    let c = decode(Config, account_info(config).data);
    assert c.fee_bps == 25 && c.label == null;
  }
}
```

The account holds 35 bytes: 32 for `admin`, 2 for `fee_bps` and 1 for the
`None` tag of the omitted `label`. Decoding it back gives `label == null`.

## Instruction data helpers

These build the `data` of common instructions for use in a
[`transaction`](../language/runtime.md#transaction). Each returns `bytes`.

| Signature | Instruction | Encoding |
|---|---|---|
| `system_transfer(lamports: u64) -> bytes` | System Program `Transfer` | `u32` index 2, then `u64` lamports |
| `spl_transfer(amount: u64) -> bytes` | SPL Token `Transfer` | `u8` 3, then `u64` amount |
| `spl_transfer_checked(amount: u64, decimals: u8) -> bytes` | SPL Token `TransferChecked` | `u8` 12, `u64` amount, `u8` decimals |
| `spl_mint_to(amount: u64) -> bytes` | SPL Token `MintTo` | `u8` 7, then `u64` amount |
| `memo(text: string) -> bytes` | Memo program | UTF-8 bytes of `text` |

The SPL Token encodings are the same for Token-2022; choose the program with
the instruction's program id. The helpers only build data: the instruction's
accounts must still be listed in the order the program expects (for example
source, destination, owner for `spl_transfer`).

## Account metas

These produce `account_meta` values for an instruction's `accounts` list, when
a plain pubkey's default flags (writable, signer if it belongs to a signer) are
not what the instruction needs.

| Signature | Writable | Signer |
|---|---|---|
| `readonly(key: pubkey) -> account_meta` | no | only if `key` belongs to a transaction signer |
| `writable(key: pubkey) -> account_meta` | yes | only if `key` belongs to a transaction signer |
| `signer(key: pubkey [, writable: bool]) -> account_meta` | `writable`, default `true` | yes |

A `pubkey` converts implicitly to `account_meta` where one is expected. The
full rules, including how signer flags are completed from `signers` and the
fee payer, are in [account metas](../language/runtime.md#account-metas).

## State queries

State queries read accounts. In `genesis` they read the state declared so far
in the same genesis block (a genesis `program` appears as an executable
account with 1 lamport, owned by its loader). In `bootstrap` and `scenario`
they query the validator over RPC at `confirmed` commitment. See
[where builtins may be used](#where-builtins-may-be-used) for the context rules.

| Signature | Result | Notes |
|---|---|---|
| `balance(key: pubkey) -> int` | lamports | `0` if the account does not exist. |
| `token_balance(key: pubkey) -> int` | token amount in base units | Fails if the account does not exist (`token account <key> does not exist`), is not owned by SPL Token or Token-2022 (`account <key> is not owned by a token program`), or is not a token account (`account <key> is not a token account`). |
| `account_info(key: pubkey) -> Option<{address: pubkey, lamports: u64, owner: pubkey, data: bytes, executable: bool, rent_epoch: u64}>` | account snapshot | `null` if the account does not exist. Reading a field through `null` is a run-time error, so check with `exists` or `!= null` first. |
| `exists(key: pubkey) -> bool` | whether the account exists | |
| `slot() -> int` | current slot | Runtime only. |
| `epoch() -> int` | current epoch | Runtime only. |

The function is called `account_info`, not `account`, because `account` is the
genesis declaration keyword. State queries pair naturally with
[`await`](../language/runtime.md#await), which re-evaluates them until a
condition holds.

## Miscellaneous

| Signature | Result | Notes |
|---|---|---|
| `len(x: string, bytes or list) -> int` | length | Strings are measured in UTF-8 bytes, not characters. |
| `str(x: any) -> string` | text form of `x` | See [str and print](#str-and-print). |
| `print(...) -> void` | nothing | Writes its arguments to standard output. |
| `rent_exempt(size: u64) -> int` | lamports | Rent-exempt minimum for an account with `size` bytes of data: `(size + 128) * 3480 * 2`. `rent_exempt(0)` is 890880, `rent_exempt(165)` is 2039280. |
| `sol(n: number) -> int` | lamports | `n * 1_000_000_000`. A float must give a whole number of lamports (`sol(1.5)` is fine); otherwise `amount is not a whole number of lamports`. Integer overflow is an error. |
| `rpc(url: string) -> source` | an RPC endpoint | See [rpc](#rpc). |

### str and print

`str` and `print` use the same text form for each kind of value:

| Value | Text |
|---|---|
| integers, floats, `true`/`false`, `null` | `42`, `1.25`, `true`, `null` |
| `string` | the string itself (quoted when inside a list or object) |
| `bytes` | `0x` followed by lowercase hex, e.g. `0x0a0b`; empty is `0x` |
| `pubkey` | base58 |
| `keypair` | `keypair(<base58 pubkey>)` |
| `duration` | milliseconds, e.g. `2000ms` |
| `source` | `source(<url>)` |
| lists, objects | `["a", "b"]`, `{x: 1, y: "s"}` |
| `account_meta` | `readonly(<pk>)`, `writable(<pk>)` or `signer(<pk>)` |

`print` takes any number of arguments of any type, joins their text forms with
single spaces and ends the line; `print()` prints an empty line. It returns
nothing, so it can only be used as a statement:

```text
printv.tid:1:9: error: initializer of 'v' has no value (the expression returns nothing)
 1 | let v = print("x");
   |         ^~~~~
1 error(s), 0 warning(s)
```

`str` is useful for building messages and seed names, because `+` on strings
requires both sides to be strings:

```tide
let summary = "fee=" + str(c.fee_bps) + " admin=" + str(c.admin);
print(summary);
```

With the `Config` account above, this prints `fee=25 admin=` followed by
alice's base58 address. Note that `str(kp)` of a keypair gives
`keypair(...)`; use `str(kp.pubkey)` for the bare address.

### len

`len` accepts a `string`, `bytes` or a list and returns an `int`. Any other
type is a check-time error:

```text
lenint.tid:1:13: error: 'len' expects a string, bytes or list, found int
 1 | let n = len(5);
   |             ^
1 error(s), 0 warning(s)
```

### rpc

`rpc(url)` wraps a URL as a `source`, the type expected by
[genesis](../language/genesis.md) `fork` and `clone`. A plain string is also
accepted wherever a source is expected, and `source name = "...";` bindings
work too. The URL is available as `src.url`.

When the source is used by `fork` or `clone`, these short names are expanded:

| Name | URL |
|---|---|
| `mainnet`, `mainnet-beta`, `m` | `https://api.mainnet-beta.solana.com` |
| `devnet`, `d` | `https://api.devnet.solana.com` |
| `testnet`, `t` | `https://api.testnet.solana.com` |
| `localhost`, `l` | `http://127.0.0.1:8899` |

Any other string is used as the URL unchanged. `src.url` returns the text as
written, so `rpc("devnet").url` is `"devnet"`.

```tide
source main = rpc("mainnet");
```

See also: [schemas](../language/schemas.md) for schema layouts,
[runtime statements](../language/runtime.md) for `transaction`, `invoke` and
`await`, and [types and values](../language/types-and-values.md).
