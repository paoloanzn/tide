# Schemas

Solana account data and instruction data are plain byte arrays. Programs
give them meaning through a binary layout, usually Borsh, with an 8-byte
discriminator in front for Anchor programs. Writing these bytes by hand with
`hex(...)` and `u64_le(...)` is error-prone. A `schema` declares the layout
once. Tide then encodes objects to bytes (`borsh`, `anchor`), decodes bytes
back to objects (`decode`), and checks object literals against the layout
when you run `tide check`.

## Declaring a schema

```tide
schema Position using borsh {
  x: i32;
  y: i32;
}
```

The syntax is `schema <Name> using <format> { <field>: <type>; ... }`.

- `<format>` is `borsh` or `anchor`. Both use the Borsh encoding for the
  fields. `anchor` also marks the schema as an Anchor account type, whose
  encoding starts with an 8-byte discriminator (see [anchor()](#anchor)).
- Fields are encoded in declaration order. Field names must be unique within
  the schema.
- Schemas are top-level declarations. Like every name, a schema must be
  declared before it is used, including inside another schema.
- An imported schema is used as `lib.Name` in expressions such as
  `borsh(lib.Name, ...)`.

A schema name is also a type: it describes an object with exactly the
schema's fields. You can use it in `let` annotations and function parameters
(`fn encode(p: Position) -> bytes`). Qualified names such as `lib.Name` are
not accepted in type annotations.

Errors in the declaration:

```tide
schema Bad using borsh {
  count: int;
  key: keypair;
  when: duration;
}
```

```text
net.tid:2:10: error: schema fields need a sized integer type (u8, u16, u32, u64, u128, i8, ..., i128)
 2 |   count: int;
   |          ^~~
net.tid:3:8: error: type 'keypair' cannot be used in a schema (it has no binary encoding)
 3 |   key: keypair;
   |        ^~~~~~~
net.tid:4:9: error: type 'duration' cannot be used in a schema (it has no binary encoding)
 4 |   when: duration;
   |         ^~~~~~~~
3 error(s), 0 warning(s)
```

A format other than `borsh` or `anchor` is an error
(`unknown schema format 'json'; expected 'borsh' or 'anchor'`).

## Field types

Schema fields need types with a fixed binary encoding. The unbounded `int`
type, and `keypair`, `source`, `duration`, `range` and `account_meta`, are
rejected.

| Type | Encoding | Written as |
|---|---|---|
| `u8` `u16` `u32` `u64` `u128` | little-endian unsigned, 1/2/4/8/16 bytes | integer, range-checked |
| `i8` `i16` `i32` `i64` `i128` | little-endian two's complement | integer, range-checked |
| `bool` | 1 byte, `0` or `1` | `true` / `false` |
| `f32`, `f64` (or `float`) | IEEE 754, 4 or 8 bytes | number (integers are converted) |
| `string` | `u32` byte length, then UTF-8 bytes | string |
| `bytes` | `u32` length, then the bytes (same as `[u8]`) | `bytes` value or list of integers |
| `pubkey` | 32 bytes | `pubkey` |
| `Option<T>` | `0` for none; `1` followed by `T` for some | `null` or a `T` value |
| `[T]` | `u32` element count, then each element | list |
| `[T; N]` | exactly `N` elements, no length prefix | list of length `N` |
| `(T, U, ...)` | each element in order, no prefix | list literal, e.g. `[7, "z"]` |
| another schema | its fields in order, no discriminator | object |

Tuple types are only allowed in schema fields. Elsewhere they are an error
(`tuple types are only allowed in schema fields`).

### Example

```tide
keypair alice = seed("alice");

schema Position using borsh {
  x: i32;
  y: i32;
}

schema Profile using borsh {
  owner: pubkey;
  level: u8;
  name: string;
  home: Position;                 // nested schema
  scores: [u16];                  // u32 length prefix, then elements
  flags: [bool; 2];               // fixed length, no prefix
  nickname: Option<string>;       // 0 = None, 1 + value = Some
  pair: (u8, string);             // tuple, written as a list literal
}

let p = borsh(Profile, {
  owner: alice.pubkey,
  level: 3,
  name: "al",
  home: { x: -1, y: 2 },
  scores: [10, 20],
  flags: [true, false],
  pair: [7, "z"],
});
```

`p` is 64 bytes: 32 (owner) + 1 (level) + 6 (`name`: 4-byte length and 2
bytes) + 8 (`home`) + 8 (`scores`: 4-byte count and two `u16`) + 2 (`flags`)
+ 1 (`nickname`, omitted, so none) + 6 (`pair`). For a smaller case,
`borsh(Position, { x: 1, y: -1 })` is `0x01000000ffffffff`.

A nested schema contributes only its fields. If the nested schema is declared
`using anchor`, its discriminator is not written; discriminators appear only
at the start of a top-level `anchor` encoding.

## borsh()

`borsh(Schema, obj)` returns the encoding of `obj` as `bytes`:

```tide
account pda(counter.pubkey, ["config"]) {
  lamports = rent_exempt(40);
  owner = counter.pubkey;
  data = borsh(Note, { author: admin.pubkey, amount: 7 });
}
```

When `Schema` is declared `using anchor`, `borsh()` also writes the Anchor
discriminator first, so `borsh(S, obj)` and `anchor(S, obj)` give the same
bytes for an `anchor` schema. For instruction data and other layouts without a
discriminator, declare the schema `using borsh`.

## anchor()

Anchor accounts start with an 8-byte discriminator that identifies the
account type. `anchor(Schema, obj)` writes that discriminator, then the Borsh
encoding of `obj`:

```tide
keypair admin = seed("admin");

schema Vault using anchor {
  authority: pubkey;
  balance: u64;
}

let v = anchor(Vault, { authority: admin.pubkey, balance: 5 });   // 8 + 32 + 8 = 48 bytes
```

The discriminator is the first 8 bytes of `sha256("account:<Name>")`, where
`<Name>` is the schema's name, as Anchor computes it for an account struct of
that name. For `Vault`, `sha256("account:Vault")` starts with
`d308e82b02987577`, so `v` starts with those bytes. `anchor()` writes the
discriminator for any schema, whatever its `using` format, so name the
schema exactly like the Rust account struct.

### Anchor IDL account types

`anchor("Name", obj)` takes the account type's name as a string. Tide looks
the name up in this order:

1. A schema called `Name` (in the script or its imports). This works like
   `anchor(Name, obj)`, and the object literal is checked when the name is a
   string literal.
2. The `accounts` of the Anchor IDLs attached to programs declared in
   genesis with `idl = "..."` (see [program](genesis.md#program)). Tide uses
   the layout from the IDL and its `discriminator` array (Anchor 0.30 and
   later IDLs), or `sha256("account:Name")` for older IDLs that have none.

```tide
keypair admin = seed("admin");
keypair counter = seed("counter");

network n {
  genesis {
    program counter {
      id = counter.pubkey;
      binary = "target/deploy/counter.so";
      idl = "target/idl/counter.json";
    }
    account pda(counter.pubkey, ["state"]) {
      lamports = rent_exempt(16);
      owner = counter.pubkey;
      data = anchor("Counter", { value: 7 });
    }
  }
}
```

Given an IDL that defines a `Counter` account with fields `value: u64` and
the discriminator `[255, 176, 4, 245, 188, 253, 124, 25]`, the data is
`0xffb004f5bcfd7c190700000000000000`.

IDL-based encoding happens when the script runs, not at check time, so
mistakes are reported by `tide up`:

```text
net.tid:12:32: error: cannot encode Counter: unknown field 'valu' for Counter
 12 |       data = anchor("Counter", { valu: 7 });
    |                                ^
1 error(s), 0 warning(s)
```

Notes on IDL encoding:

- The program must be declared earlier in genesis. At top level no IDL is
  known yet, and the name must match a schema:
  `unknown anchor account type 'Counter': declare 'schema Counter using anchor { ... }' or give the owning program an 'idl' in genesis`.
- Supported IDL types: integers, `bool`, `f32`, `f64`, `string`, `bytes`,
  `publicKey`/`pubkey`, `vec`, `option`, `coption`, fixed `array`, `defined`
  structs and enums whose variants carry no data (written as the variant name
  string). Tuple structs and enum variants with data are not supported.
- Object keys match IDL field names in either snake_case or camelCase
  (`new_value` matches `newValue`).

## decode()

`decode(Schema, data)` reads `bytes` back into an object whose type is the
schema, so its fields can be read with `.`:

```tide
let back = decode(Profile, p);
print(back.home.x, back.scores, back.nickname);   // -1 [10, 20] null
```

- For a schema declared `using anchor`, `decode` first checks and skips the
  8-byte discriminator.
- Bytes after the last field are ignored, so you can decode a prefix of a
  larger account.
- Too little data, an invalid `bool` or `Option` tag, or a wrong discriminator
  is an error when the script runs:

```text
net.tid:5:26: error: data does not start with the anchor discriminator of 'Tagged'
 5 |   let t = decode(Tagged, hex("0000000000000000"));
   |                          ^~~
1 error(s), 0 warning(s)
```

```text
net.tid:2:44: error: cannot decode data as 'Pair': unexpected end of data
 2 | network n { genesis { let p = decode(Pair, hex("01")); } }
   |                                            ^~~
1 error(s), 0 warning(s)
```

`decode` is pure: it works anywhere expressions are allowed. A common use is
reading program state in a scenario:

```tide
let note = decode(Note, account_info(note_address).data);
assert note.amount == 42 && note.memo == null;
```

`decode` takes a schema, not a string; to decode an IDL-only type, declare a
matching schema.

## Compile-time checking

When the object passed to `borsh`, `anchor` or a schema-typed binding is an
object literal, `tide check` checks it against the schema: unknown fields
(with suggestions), missing fields, and the type and range of each value,
including nested objects, fixed-length arrays and tuples.

```tide
schema Config using borsh {
  admin: pubkey;
  fee_bps: u16;
  label: Option<string>;
}
let a = borsh(Config, { admin: seed("a").pubkey, fee_bp: 30 });
let b = borsh(Config, { admin: seed("a").pubkey, fee_bps: 70000 });
```

```text
net.tid:6:50: error: unknown field 'fee_bp' for schema 'Config'; did you mean 'fee_bps'?
 6 | let a = borsh(Config, { admin: seed("a").pubkey, fee_bp: 30 });
   |                                                  ^~~~~~
net.tid:6:23: error: missing field 'fee_bps' for schema 'Config'
 6 | let a = borsh(Config, { admin: seed("a").pubkey, fee_bp: 30 });
   |                       ^
net.tid:7:59: error: value out of range for u16 in field 'fee_bps' of schema 'Config'
 7 | let b = borsh(Config, { admin: seed("a").pubkey, fee_bps: 70000 });
   |                                                           ^~~~~
3 error(s), 0 warning(s)
```

### Omitted Option fields

A field of type `Option<T>` may be left out of an object literal. It is
encoded as none, exactly like writing `null`:

```tide
let a = borsh(Config, { admin: seed("a").pubkey, fee_bps: 30 });
let b = borsh(Config, { admin: seed("a").pubkey, fee_bps: 30, label: null });
let c = borsh(Config, { admin: seed("a").pubkey, fee_bps: 30, label: "main" });
// a == b; len(a) is 35, len(c) is 43
```

### Objects stored in variables

The check applies to literals. An object literal stored in an unannotated
`let` gets a plain object type, which must then match the schema's fields
exactly, so an omitted `Option` field no longer matches:

```tide
schema Config using borsh { admin: pubkey; fee_bps: u16; label: Option<string>; }
let o = { admin: seed("a").pubkey, fee_bps: 30 };
let a = borsh(Config, o);
```

```text
net.tid:3:23: error: type mismatch in argument 2 of 'borsh': expected Config, found {admin: pubkey, fee_bps: int}
 3 | let a = borsh(Config, o);
   |                       ^
1 error(s), 0 warning(s)
```

Annotate the binding with the schema type instead, so the literal is checked
(and its omitted options accepted) where it is written:

```tide
let o: Config = { admin: seed("a").pubkey, fee_bps: 30 };
let a = borsh(Config, o);
let c = decode(Config, a);
let d = borsh(Config, c);    // decode results already have the schema type
```

See also: [types and values](types-and-values.md) for objects, lists and
`Option`, [genesis](genesis.md) for putting encoded data into accounts, and
the [standard library](../reference/standard-library.md) for the byte helpers
(`hex`, `u64_le`, `sha256`, ...).
