# Types and values

Tide is statically typed. Every expression has a type that `tide check`
determines before anything runs, so most mistakes (a string where a pubkey is
expected, a misspelled field, a `u8` literal of 300) are reported without
starting a validator. This page lists the types, how literals and units become
values, which conversions happen implicitly, and how values behave when they are
copied.

Type annotations are written after a `:` in `let` declarations and function
parameters, and after `->` for function return types (see
[Declarations and scope](declarations-and-scope.md)). Most of the time they can
be omitted: a `let` variable takes the type of its initializer.

## Overview

| Type | Values | Written as a literal | Notes |
|---|---|---|---|
| `int` | 128-bit signed integers | `42`, `1.5 SOL`, `10 slots` | checked arithmetic |
| `u8` ... `u128`, `i8` ... `i128` | `int` restricted to a range | (same as `int`) | range-checked, see [Sized integers](#sized-integers) |
| `float` (alias `f64`), `f32` | 64-bit floating point | `1.5` | `f32` only changes serialization |
| `bool` | `true`, `false` | `true` | |
| `string` | UTF-8 text | `"abc"` | |
| `bytes` | byte strings | (none; use `hex("...")` etc.) | |
| `duration` | milliseconds | `250 ms`, `2 s` | |
| `pubkey` | 32-byte Solana addresses | (none; use `pubkey("...")`, `kp.pubkey`) | |
| `keypair` | ed25519 keypairs | (none; use `seed("...")`) | has `.pubkey` |
| `source` | RPC endpoints | (none; use `rpc("...")` or a URL string) | has `.url` |
| `range` | integer ranges | `0..10`, `1..=3` | iterable |
| `[T]`, `[T; N]` | lists | `[1, 2, 3]` | `[T; N]` has a fixed length |
| `Option<T>` | `null` or a `T` | `null` | |
| object types | records with named fields | `{ name: "a", n: 1 }` | no annotation syntax |
| `(T, U, ...)` | tuples | `[1, true]` | schema fields only |
| `account_meta` | an account reference for an instruction | (none; use `readonly(pk)` etc.) | |
| schema names | records described by a `schema` | `{ ... }` | see [Schemas](schemas.md) |

The builtins mentioned in this table are documented in the
[standard library reference](../reference/standard-library.md).

## Integers

### `int`

`int` is a signed 128-bit integer. All arithmetic on integers is checked:
overflow, division by zero and modulo by zero stop execution with a positioned
error instead of wrapping or producing garbage. This matters because amounts in
Solana are `u64` lamports and token units; silent wraparound would produce a
genesis that looks valid but holds the wrong balances.

- `/` truncates toward zero: `7 / 2` is `3`, `-7 / 2` is `-3`.
- `%` takes the sign of the left operand: `-7 % 2` is `-1`.

Errors in integer arithmetic on computed values are found when the code runs
(`tide up`), not by `tide check`:

```tide
let max = 170141183460469231731687303715884105727;
let boom = max + 1;
```

```text
overflow.tid:2:16: error: integer overflow
 2 | let boom = max + 1;
   |                ^
```

### Sized integers

The names `u8`, `u16`, `u32`, `u64`, `u128`, `i8`, `i16`, `i32`, `i64` and
`i128` are refinements of `int`, not separate representations. At runtime every
integer is 128-bit; a sized type adds a range that is checked whenever a value
is stored into a slot of that type.

| Type | Range |
|---|---|
| `u8`, `u16`, `u32`, `u64` | 0 to 2^bits - 1 |
| `i8`, `i16`, `i32`, `i64`, `i128` | -2^(bits-1) to 2^(bits-1) - 1 |
| `u128` | 0 to 2^127 - 1 (values are 128-bit signed at runtime) |

A sized slot is any of: an annotated `let` variable (or one whose initializer
has a sized type), a function parameter or return type, a field of a genesis
declaration (for example `lamports` is `u64`), a builtin argument (for example
`u8(n)`), and a schema field during serialization.

When the check happens:

- **At check time**, when the value is an integer literal (optionally negated)
  and the slot type is known:

  ```tide
  let a: u8 = 256;
  ```

  ```text
  range.tid:1:13: error: value out of range for u8 in initializer of 'a'
   1 | let a: u8 = 256;
     |             ^~~
  ```

- **At runtime**, for computed values: on a `let` initializer with an
  annotation, on every assignment (including `+=` and friends), on argument
  passing and return, and when a value is written into a field or serialized.

  ```tide
  let big = 300;
  let small: u8 = big;
  ```

  ```text
  range_rt.tid:2:17: error: initializer of 'small': 300 is out of range for u8
   2 | let small: u8 = big;
     |                 ^~~
  ```

Arithmetic itself is not range-checked against the operand types: the result
of an operator is always a plain `int`. With `let a: u8 = 200;`, the expression
`a + a` is the `int` 400. Only storing it back into a `u8` slot fails.

Integer types are freely assignable to each other at check time; the range
check is what keeps a value inside its slot.

## Floats

`float` (also spelled `f64`) is an IEEE 754 double. `f32` is the same type in
expressions; it only differs when a value is serialized through a
[schema](schemas.md), where it is written as 4 bytes.

Floats exist mainly for configuration values such as `faucet_sol` and the rent
`exemption_threshold`, and for SOL amounts computed with `sol(n)`. Integer
arithmetic is preferred everywhere else because it is exact.

- An `int` converts implicitly to `float` (`let f: float = n;`), and an integer
  literal in a float context is a float literal (`let f: float = 1;`).
- A `float` never converts implicitly to an `int`. There is no truncating
  conversion builtin in v0.1.0; use `sol(n)` to turn a SOL amount into lamports.
- Mixing `int` and `float` in arithmetic produces a `float`: `1 + 2.5` is `3.5`.
- Float division by zero is an error, like integer division.

```tide
let f = 2.5;
let i: int = f;
```

```text
float.tid:2:14: error: type mismatch in initializer of 'i': expected int, found float
 2 | let i: int = f;
   |              ^
```

## Units and check-time folding

Unit suffixes ([lexical syntax](lexical-structure.md#unit-suffixes)) are
resolved by the checker, which turns the literal into an exact value using
decimal arithmetic (no floating-point rounding):

| Literal | Type | Value |
|---|---|---|
| `42`, `42 lamports`, `42 slots` | `int` | 42 |
| `1.5 SOL` | `int` | 1 500 000 000 |
| `250 ms` | `duration` | 250 ms |
| `1.5 s` | `duration` | 1500 ms |
| `1.5` | `float` | 1.5 |

`SOL` and `lamports` produce plain `int` values, not a distinct "amount" type,
so `1 SOL + 5000 lamports` is ordinary integer addition. `slots` is likewise
only a readability marker.

The result must be a whole number of the base unit (lamports or
milliseconds). Trailing zeros after the decimal point are ignored, so
`1.50 SOL` is fine. A literal that would need a fractional lamport or
millisecond is rejected:

```tide
let a = 1.5 lamports;
let b = 0.0000000001 SOL;
let c = 1.0005 s;
```

```text
units.tid:1:9: error: '1.5' is not a whole number of lamports
 1 | let a = 1.5 lamports;
   |         ^~~~~~~~~~~~
units.tid:2:9: error: '0.0000000001' is not a whole number of lamports
 2 | let b = 0.0000000001 SOL;
   |         ^~~~~~~~~~~~~~~~
units.tid:3:9: error: '1.0005' is not a whole number of milliseconds
 3 | let c = 1.0005 s;
   |         ^~~~~~~~
```

A folded value that does not fit in 128 bits is reported as
`integer literal is too large`.

## Booleans and strings

`bool` has the values `true` and `false`. Conditions in `if`, `while`, `assert`
and `await` must be `bool`; there is no truthiness for numbers, strings or
`null`.

`string` holds UTF-8 text. Strings can be concatenated with `+` and compared
with `==`, `!=`, `<`, `<=`, `>`, `>=` (byte-wise lexicographic order). They
cannot be indexed, and `+` does not convert other types: use `str(x)` to turn a
value into its text form.

```tide
let n = 3;
let label = "count: " + str(n);
```

## Bytes

`bytes` is a byte string, used for account data and instruction data. There is
no bytes literal; bytes come from builtins such as `hex("0a0b")`,
`base64(...)`, `base58(...)`, `utf8("text")`, `file(path)`, `sha256(x)`, the
little-endian encoders `u8(n)`, `u16_le(n)` ... `i64_le(n)`, and schema
encoding with `borsh(...)` / `anchor(...)`.

- `+` concatenates two `bytes` values.
- Indexing a `bytes` value gives a `u8`; assigning to an index stores a byte
  (values outside 0 to 255 are an error).
- `bytes` and `[u8]` convert implicitly in both directions, so a list literal of
  small integers can be used where `bytes` is expected: `let b: bytes = [1, 2, 3];`.
- `len(b)` gives the number of bytes.

## Durations

`duration` is a span of time in whole milliseconds (stored as a signed 64-bit
count). Duration literals use the `ms` and `s` units. Durations are used for
timeouts such as `confirm_timeout` in a network's `options`
([Networks](networks.md)).

| Operation | Result |
|---|---|
| `duration + duration`, `duration - duration` | `duration` |
| `duration * int`, `int * duration` | `duration` |
| `duration / int` | `duration` (truncated) |
| `-duration` | `duration` |
| `<`, `<=`, `>`, `>=`, `==`, `!=` between durations | `bool` |

A duration cannot be combined with a plain number by `+` or `-`
(`2 s + 500` is an error; write `2 s + 500 ms`).

## Pubkeys, keypairs and sources

### `pubkey`

A Solana address. Pubkeys are produced by `pubkey("base58...")` (a literal
string is validated at check time), by `kp.pubkey` on a keypair, by `pda(...)`,
`ata(...)` and the well-known program ids such as `system_program()`. Pubkeys
support `==` and `!=` only.

### `keypair`

An ed25519 keypair, created with `seed("name")` (deterministic, see
[Declarations and scope](declarations-and-scope.md#determinism)) or
`keypair_file(path)` (a Solana CLI JSON keypair file). A keypair has one member:

| Member | Type |
|---|---|
| `kp.pubkey` | `pubkey` |

Keypairs cannot be compared with `==` or `!=` (compare their `.pubkey`
instead), and they cannot appear in schemas.

### `source`

An RPC endpoint used by `fork` and `clone` in genesis. Created with
`rpc(url)`. The URL may also be a cluster name such as `"mainnet"`, `"devnet"`,
`"testnet"` or `"localhost"`, which the backend resolves when it connects (see
the [standard library](../reference/standard-library.md)). Wherever a `source` is expected (a `source` declaration, `fork
from`, `clone ... from`) a plain URL string is accepted too.

| Member | Type |
|---|---|
| `src.url` | `string` |

Accessing any other member is an error with a suggestion:

```tide
keypair k = seed("k");
let p = k.pubky;
```

```text
member.tid:2:11: error: type keypair has no member 'pubky'; did you mean 'pubkey'?
 2 | let p = k.pubky;
   |           ^~~~~
```

## Ranges

`a..b` is the integers from `a` up to but not including `b`; `a..=b` includes
`b`. Both ends must be `int`. A range is a value of type `range`: it can be
stored in a variable and iterated with `for` (see
[Expressions and statements](expressions-and-statements.md#for)). A range whose
end is not greater than its start is empty.

## Lists

`[T]` is a list of `T`. `[T; N]` is a list with exactly `N` elements, where `N`
must be a non-negative integer literal. Lists are written as literals with
square brackets; a trailing comma is allowed.

```tide
let amounts = [1 SOL, 2 SOL, 3 SOL];      // [int]
let mixed = [1, 2.5];                     // [float]: int widens to float
let maybe = [null, seed("a").pubkey];     // [Option<pubkey>]
let fixed: [u8; 4] = [1, 2, 3, 4];
let none: [pubkey] = [];                  // annotate empty lists
```

- The element type of an unannotated list literal is the common type of its
  elements: `int` and `float` join to `float`, and `null` with `T` joins to
  `Option<T>`. Other combinations are an error
  (`list elements have incompatible types int and string`).
- Annotate an empty list. Without an annotation `[]` has no useful element
  type.
- A fixed length is checked at check time for list literals
  (`expected 4 elements for [u8; 4], found 3`) and at runtime for computed
  values.
- Indexing with `xs[i]` uses an `int` index; an index outside the list is a
  runtime error (`index 3 is out of bounds for a list of length 3`).
- `+` concatenates two lists, `len(xs)` gives the length.

## `Option<T>` and null

`Option<T>` holds either `null` or a value of type `T`. It exists for values
that may legitimately be absent: an optional authority (`freeze_authority =
null;`), the result of `account_info(pk)` for an account that does not exist,
or an optional schema field.

- A `T` and `null` both convert implicitly to `Option<T>`.
- `Option<Option<T>>` is the same type as `Option<T>`.
- `null` alone has no type to infer from, so `let x = null;` needs an
  annotation:

  ```text
  null.tid:1:9: error: cannot infer the type of 'x' from null; add a type annotation such as ': Option<pubkey>'
   1 | let x = null;
     |         ^~~~
  ```

- Compare with `null` using `==` or `!=`.
- Member access goes through the option: if `info` is
  `Option<{lamports: u64, ...}>`, then `info.lamports` has type `u64`. If `info`
  is `null` at runtime, the access fails with `cannot read member 'lamports' of
  null`. Check `info != null` first.
- There is no unwrap operator in v0.1.0. An `Option<int>` cannot be used where
  an `int` is expected, and operators do not apply to it:

  ```text
  option.tid:2:11: error: operator '+' cannot be applied to Option<int> and int
   2 | let y = x + 1;
     |           ^
  ```

```tide
network demo {
  genesis {
    let info = account_info(seed("maybe").pubkey);
    if info != null {
      print(info.lamports);
    }
  }
}
```

## Object types

An object literal `{ key: value, ... }` creates a record. Keys are identifiers
(reserved words allowed) or string literals; duplicate keys are an error. The
type of `{ name: "a", amount: 1 }` is written `{name: string, amount: int}` in
diagnostics.

There is no syntax for writing an object type in an annotation. Record types
come from object literals, from [schemas](schemas.md) and from builtins:

| Source | Record type |
|---|---|
| `account_info(pk)` | `Option<{address: pubkey, lamports: u64, owner: pubkey, data: bytes, executable: bool, rent_epoch: u64}>` |
| `transaction as tx { ... }` | `tx : {success: bool, signature: string, error: string, logs: [string], slot: u64}` (see [Runtime](runtime.md)) |
| `decode(Schema, bytes)` | the schema's record type |

Two object types are compatible when they have the same field names (in any
order) with compatible field types. Fields are read with `obj.field` and, on a
mutable variable, assigned with `obj.field = value;`.

## Tuples

`(T, U, ...)` types exist only for [schema](schemas.md) fields, to describe
Borsh tuples. Tuple values are written as list literals (`[1, true]`). Using a
tuple type anywhere else is an error
(`tuple types are only allowed in schema fields`).

## `account_meta`

An `account_meta` describes how an instruction uses an account: its pubkey and
whether it is a signer and writable. It is produced by `readonly(pk)`,
`writable(pk)` and `signer(pk [, writable])`. A plain `pubkey` converts
implicitly to `account_meta`, which is what lets an instruction's `accounts`
list mix bare pubkeys and explicit metas; the flags a bare pubkey gets are
described in [Runtime](runtime.md).

## Schema types

A `schema` declaration names a record layout. The schema name can be used as a
type annotation, and an object literal checked against it must have exactly the
declared fields (fields of an `Option` type may be omitted). Schema types are
nominal: two schemas with identical fields are still different types. Schemas
and their encodings are covered in [Schemas](schemas.md).

Schema fields need types with a binary encoding: `int` must be a sized type,
and `duration`, `keypair`, `source`, `range` and `account_meta` are not
allowed.

## Implicit conversions

Values convert implicitly only in these cases. Everything else must match
exactly, and a mismatch is reported as
`type mismatch in <context>: expected X, found Y`.

| From | To | Notes |
|---|---|---|
| `int` (any width) | `float`, `f32` | |
| `int` (any width) | another integer width | range-checked on store |
| `T` | `Option<T>` | |
| `null` | `Option<T>` | |
| `bytes` | `[u8]` (any integer element type) | |
| `[int]` | `bytes` | each element must be 0 to 255 |
| `pubkey` | `account_meta` | |
| `[T]` | `[U]` | when `T` converts to `U` |
| `[T]` | `[T; N]` | length checked |

## Copy semantics

Values behave as if they were copied on every assignment, binding, argument
pass and return. Changing a list or object through one variable never affects
another variable.

```tide
network demo {
  genesis {
    let a = [1, 2, 3];
    let b = a;
    b[0] = 99;
    print(a, b);   // [1, 2, 3] [99, 2, 3]

    let o = { x: 1, y: [1] };
    let p = o;
    p.y[0] = 5;
    print(o, p);   // {x: 1, y: [1]} {x: 1, y: [5]}
  }
}
```

This makes it safe to build a template object, copy it, and change a field per
use. (The implementation shares the storage until one copy is modified, so
copying a large list is cheap.)

## Equality

`==` and `!=` compare values structurally: lists element by element, objects
field by field (order of fields does not matter), byte strings byte by byte.
An `int` compares equal to a `float` with the same numeric value, and a
`bytes` value compares equal to a `[u8]` list with the same contents. The two
operand types must be compatible at check time, and keypairs cannot be compared.
See [Expressions and statements](expressions-and-statements.md#comparison-and-equality).

Next: [Expressions and statements](expressions-and-statements.md).
