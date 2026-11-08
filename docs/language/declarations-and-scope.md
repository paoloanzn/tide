# Declarations and scope

This page covers the structure of a Tide file: the declarations allowed at top
level, where each name is visible, which bindings can change, how functions and
imports work, and in what order a script is evaluated. The goal of these rules
is that a script reads top to bottom and produces the same network every time
it runs.

## Top-level declarations

A `.tid` file is a sequence of declarations. Only these are allowed at top
level:

| Declaration | Form | Purpose |
|---|---|---|
| `import` | `import "path.tid" as name;` | use another file's declarations ([Imports](#imports)) |
| `let` | `let name [: T] = expr;` | a named value |
| `keypair` | `keypair name = expr;` | a named keypair |
| `source` | `source name = expr;` | a named RPC endpoint |
| `schema` | `schema Name using borsh { ... }` | a binary layout ([Schemas](schemas.md)) |
| `fn` | `fn name(params) [-> T] { ... }` | a function ([Functions](#functions)) |
| `network` | `network name { ... }` | a network definition ([Networks](networks.md)) |

Statements such as `assert`, `if` or a bare call are not declarations and
cannot appear at top level:

```text
top.tid:1:1: error: expected a declaration (import, source, keypair, let, schema, fn or network)
 1 | assert true;
   | ^
```

### `let`, `keypair` and `source`

The three binding forms differ in what they hold and whether they can change:

- `let` binds any value. Its type is the annotation if present, otherwise the
  type of the initializer ([Types and values](types-and-values.md)).
- `keypair name = expr;` requires `expr` to be a `keypair`, usually
  `seed("...")` or `keypair_file("...")`.
- `source name = expr;` requires a `source` (from `rpc(url)`) or a URL string.

```tide
keypair admin = seed("admin");
source mainnet = rpc("mainnet");
let fee_payer = admin.pubkey;
let airdrop_amount: u64 = 10 SOL;

network local {
  genesis {
    account fee_payer { lamports = airdrop_amount; }
  }
}
```

`keypair` and `source` declarations are allowed at top level and directly
inside a `network`, not inside blocks:

```text
kp.tid:1:23: error: 'keypair' declarations are only allowed at top level or directly inside a network
 1 | network n { genesis { keypair k = seed("k"); } }
   |                       ^
```

Inside blocks, use `let k = seed("k");` instead.

### `schema` and `network`

A `schema` declares a Borsh or Anchor layout that `borsh(...)`, `anchor(...)`
and `decode(...)` use; see [Schemas](schemas.md). A `network` declares one
local network: its options, validator, genesis state, bootstrap and scenarios;
see [Networks](networks.md). A file may declare several networks; `tide up`
runs one of them.

## Declare before use

A name is visible only after its declaration. This applies to every kind of
declaration, including functions, and makes a file readable from top to bottom:
nothing refers forward.

```tide
let total = base + 1;
let base = 10;
```

```text
before.tid:1:13: error: unknown identifier 'base'
 1 | let total = base + 1;
   |             ^~~~
```

When the name is close to one that is visible, the error suggests it:

```text
typo.tid:2:9: error: unknown identifier 'lamports_per_usr'; did you mean 'lamports_per_user'?
 2 | let x = lamports_per_usr * 2;
   |         ^~~~~~~~~~~~~~~~
```

The same rule holds inside a network: a network-level `let` written after the
`genesis` block is not visible in that block.

## Scopes and shadowing

Names live in nested scopes:

| Scope | Contains | Parent |
|---|---|---|
| file | top-level declarations and import aliases | none |
| network | network-level `let`, `keypair`, `source` | file |
| block (`genesis`, `bootstrap`, `scenario`, `if`/`else` branch, loop body) | `let` variables and `transaction as` bindings declared in it | the enclosing scope |
| function | parameters and the function body's variables | file |

Rules:

- Declaring a name twice in the same scope is an error, whatever kinds the two
  declarations are (a `let` and a `schema` with the same name clash):

  ```text
  redecl.tid:2:5: error: redeclaration of 'x'
   2 | let x = 2;
     |     ^
  redecl.tid:1:5: note: previous declaration is here
   1 | let x = 1;
     |     ^
  ```

- Declaring a name that already exists in an enclosing scope is allowed and
  shadows the outer name until the end of the inner block:

  ```tide
  let x = 1;
  network n {
    genesis {
      let x = "inner";          // shadows the top-level x
      if true { let x = 3.5; }  // shadows again inside the branch
    }
  }
  ```

- A function's parameters and the top level of its body share one scope, so a
  body variable cannot reuse a parameter name. Likewise a loop variable and the
  top level of the loop body share one scope.
- Variables declared in one block are not visible in another. A `let` in
  `genesis` cannot be read in a `scenario`; declare it at network level to share
  it ([Evaluation order](#evaluation-order)).
- Network names, scenario names, validator names and `program` labels in genesis
  are separate namespaces. A genesis block can declare `program counter { ... }`
  next to a top-level `keypair counter`.

## Immutability

Only `let` variables can be assigned, and only where their owner allows it:

| Binding | Mutable? |
|---|---|
| top-level `let` | never (top level holds no assignment statements, and nothing else may assign it) |
| network-level `let` | yes, in that network's genesis, bootstrap and scenarios |
| block `let` | yes, within its scope |
| `keypair`, `source` | never |
| function parameter | never |
| loop variable | never |
| `transaction as name` result | never |
| import alias and its members | never |

Top-level values are fixed once they are computed, so functions and every
network see the same values.

```tide
let count = 0;
network n { genesis { count += 1; } }
```

```text
top_mut.tid:2:23: error: cannot assign to top-level variable 'count' here; top-level bindings are immutable outside top-level code
 2 | network n { genesis { count += 1; } }
   |                       ^~~~~
top_mut.tid:1:5: note: declared here
 1 | let count = 0;
   |     ^~~~~
```

```text
kp_mut.tid:2:23: error: cannot assign to keypair 'alice'; only 'let' variables are mutable
 2 | network n { genesis { alice = seed("bob"); } }
   |                       ^~~~~
kp_mut.tid:1:9: note: declared here
 1 | keypair alice = seed("alice");
   |         ^~~~~
```

## Functions

Functions package reusable computation: deriving addresses, building
instruction data, computing amounts.

```tide
fn lamports_for(sol_amount: int, accounts: int) -> int {
  return sol_amount * 1 SOL * accounts;
}

fn log(msg: string) {
  print("[setup] " + msg);
}
```

- Every parameter needs a type. Arguments are converted and range-checked
  against it when the function is called, and are passed by value
  ([copy semantics](types-and-values.md#copy-semantics)).
- `-> T` declares the return type, and the returned value is checked against
  it. Without `-> T` the function returns nothing: `return;` is allowed but
  `return expr;` is not, and calling it where a value is needed is an error
  (`initializer of 'x' has no value (the expression returns nothing)`).
- Functions are only declared at top level and are not values: they cannot be
  stored in variables or passed as arguments.

### Every path must return

A function with a return type must end in a `return` on every path. The check
is structural: the body's last statement must be a `return`, or an
`if`/`else` whose branches both end that way. Loops do not count, even
`while true`.

```tide
fn sign(x: int) -> int {
  if x > 0 { return 1; } else if x < 0 { return -1; }
}
```

```text
sign.tid:1:4: error: function 'sign' may reach the end of its body without returning a int
 1 | fn sign(x: int) -> int {
   |    ^~~~
```

The `else if` branch has no final `else`, so a path falls through. Adding
`else { return 0; }` fixes it.

### Recursion and call depth

A function can call itself, because its name is declared before its body is
checked. Mutual recursion is not possible: by the declare-before-use rule, the
first of two functions cannot see the second.

Calls nest at most 256 deep. Deeper recursion stops with an error rather than
crashing the process:

```tide
fn down(n: int) -> int { return down(n + 1); }
let x = down(0);
```

```text
depth.tid:1:33: error: maximum call depth (256) exceeded; is there unbounded recursion?
 1 | fn down(n: int) -> int { return down(n + 1); }
   |                                 ^~~~
```

Like other runtime errors, this is reported by `tide up`, not by `tide check`.

### What a function can do

A function body can see its parameters, its own local variables, and the
top-level declarations (including import aliases) that precede the function.
It cannot see network-level names: pass values from the network in as
arguments.

Within that scope, a body can contain any statement or builtin, including the
ones that touch a network. What the body does decides where the function can
be called. Network operations come in four kinds:

| Kind | Statements and builtins | The function can be called in |
|---|---|---|
| read | `balance`, `token_balance`, `account_info`, `exists` | `genesis`, `bootstrap`, `scenario` |
| write | `account`, `program`, `mint`, `token_account`, `associated_token_account`, `lookup_table`, `sysvar`, `feature`, `fork`, `clone`, `delete` | `genesis` |
| send | `transaction`, `invoke`, `airdrop`, `deploy` | `bootstrap`, `scenario` |
| live | `slot()`, `epoch()`, `await` | `bootstrap`, `scenario` |

A function does what its own lines do, plus what the functions it calls do.
It can be called only where all of that is allowed. A function that does none
of it is pure and can be called anywhere, including top-level and
network-level `let` initializers and `options` blocks.

This makes setup and test steps reusable, for example from a library:

```tide
// lib/wallets.tid

// read: callable in genesis, bootstrap and scenarios
fn funded(user: pubkey) -> bool {
  return balance(user) > 0;
}

// write: callable in genesis
fn prefund(user: pubkey) {
  account user { lamports = 10 SOL; }
}

// send: callable in bootstrap and scenarios
fn top_up(user: pubkey) {
  airdrop user amount 1 SOL;
}

// read + send: callable in bootstrap and scenarios
fn setup(user: pubkey) {
  top_up(user);
  assert funded(user);
}
```

```tide
// dev.tid
import "lib/wallets.tid" as wallets;
keypair bob = seed("bob");
network dev {
  genesis {
    wallets.prefund(bob.pubkey);
    assert wallets.funded(bob.pubkey);
  }
  bootstrap { wallets.setup(bob.pubkey); }
  scenario topped_up { wallets.top_up(bob.pubkey); }
}
```

A statement inside a function runs exactly as if it were written in the block
that called the function: in `genesis` it adds to the state being built, in
`bootstrap` and scenarios it goes to the validator.

Calling a function where it is not allowed is reported at the call. Notes
follow the calls down to the line responsible:

```tide
import "lib/wallets.tid" as wallets;
keypair bob = seed("bob");
network dev {
  genesis {
    wallets.setup(bob.pubkey);
  }
}
```

```text
bad.tid:5:5: error: 'wallets.setup' sends a transaction and cannot be called in genesis, where no network is running yet
 5 |     wallets.setup(bob.pubkey);
   |     ^~~~~~~
lib/wallets.tid:18:3: note: 'setup' calls 'top_up' here
 18 |   top_up(user);
    |   ^~~~~~
lib/wallets.tid:13:3: note: 'airdrop' sends a transaction here
 13 |   airdrop user amount 1 SOL;
    |   ^
1 error(s), 0 warning(s)
```

Genesis runs before any validator exists, and bootstrap and scenarios run
after genesis is sealed. A function that both writes genesis state and sends
transactions (or needs the running network) therefore fits nowhere. It is
reported at its definition:

```tide
fn fund_and_top_up(user: pubkey) {
  account user { lamports = 10 SOL; }
  airdrop user amount 1 SOL;
}
```

```text
both.tid:1:4: error: function 'fund_and_top_up' declares genesis state and sends a transaction, so it cannot be called anywhere: genesis declarations are only allowed in genesis, where no network is running yet
 1 | fn fund_and_top_up(user: pubkey) {
   |    ^~~~~~~~~~~~~~~
both.tid:2:3: note: 'account' declares genesis state here
 2 |   account user { lamports = 10 SOL; }
   |   ^
both.tid:3:3: note: 'airdrop' sends a transaction here
 3 |   airdrop user amount 1 SOL;
   |   ^
1 error(s), 0 warning(s)
```

Split it into two functions, one called from `genesis` and one from
`bootstrap`.

Two details:

- Program labels must be unique within one body, as in `genesis`. A function
  that declares `program vault` can still be called twice with different ids:
  both programs are named `vault` in messages. Two declarations at the same
  address are always an error.
- Relative paths, such as a program `binary`, resolve against the directory of
  the root script, even when the function is in an imported file.

## Imports

Imports split a setup across files, for example a shared file of keypairs,
schemas and helper functions used by several networks.

```tide
// lib/tokens.tid
keypair usdc_mint = seed("usdc-mint");
let decimals = 6;

schema Config using borsh { admin: pubkey; fee_bps: u16; }

fn units(whole: int) -> int {
  return whole * 1000000;
}
```

```tide
// main.tid
import "lib/tokens.tid" as tokens;

keypair admin = seed("admin");
let cfg = borsh(tokens.Config, { admin: admin.pubkey, fee_bps: 30 });

network local {
  genesis {
    print(tokens.usdc_mint.pubkey, tokens.decimals, tokens.units(5), len(cfg));
  }
}
```

Rules:

- **Path resolution.** The path is relative to the directory of the file that
  contains the `import`. An absolute path is used as is. A file that cannot be
  read is reported with the resolved path (`cannot open imported file '...'`).
- **Alias access.** Every top-level `let`, `keypair`, `source`, `schema`, `fn`
  and import alias of the imported file is available as `alias.name`. Functions
  are called as `alias.f(...)`; schemas are passed as `alias.Schema` to
  `borsh`, `anchor` and `decode`. A qualified name cannot be used as a type
  annotation (`let c: tokens.Config = ...` is a syntax error).
- **Unknown members** are reported with a suggestion
  (`module 'tokens' has no member 'decimal'; did you mean 'decimals'?`).
- **Isolation.** An imported file does not see the names of the file that
  imports it. Imported bindings cannot be assigned.
- **Loaded once.** A file imported several times, from one file or from
  several, is loaded, checked and evaluated once; all importers share it.
- **Cycles** are errors:

  ```text
  b.tid:1:8: error: import cycle: 'a.tid' is already being imported
   1 | import "a.tid" as a;
     |        ^~~~~~~
  ```

- **Networks in imported files** are type-checked like everything else, but
  they are not exported and cannot be launched: `tide up main.tid --network
  lib_only` reports `tide: no network named 'lib_only' in main.tid`. Only the
  networks of the file passed to `tide` can run.

## Evaluation order

`tide check` evaluates nothing; it parses and type-checks. When `tide up` runs a
network, evaluation happens in this order:

1. **Top-level bindings**, once: every imported file's top-level `let`,
   `keypair` and `source` declarations (imports before the files that import
   them), then those of the main file, each file in source order. Functions
   and schemas need no evaluation.
2. **Network-level bindings** of the selected network, in source order, before
   anything else in the network, wherever they appear in the block.
3. **Configuration**: the `options` and `validator` blocks.
4. **Genesis**.
5. **Bootstrap**, on a private validator instance.
6. The state is persisted and the network is launched.
7. **Scenarios**, in source order (or only those selected with `--scenario`).

Only the selected network is evaluated; other networks in the file are only
checked. The lifecycle around steps 4 to 7, including what a failure in each
step does, is described in [Runtime](runtime.md) and the
[lifecycle guide](../guides/lifecycle.md).

Network-level `let` variables are shared by all blocks of that network during
one run. A value assigned in genesis is visible in bootstrap and in the
scenarios, and a scenario's changes are visible to the scenarios after it:

```tide
network n {
  let counter = 0;
  genesis { counter += 1; }
  scenario s { counter += 10; assert counter == 11; }
}
```

Network-level initializers are evaluated before genesis exists, so like
top-level code they cannot use the state-reading builtins.

## Determinism

The same script produces the same network on every machine and every run.
Tide has no random-number or wall-clock builtins, and keys are derived from
names instead of being generated.

`seed(name)` derives an ed25519 keypair whose 32-byte seed is

```text
sha256("tide:seed:" + name)
```

where `name` is the UTF-8 string passed to `seed`. `seed("alice")` therefore
always yields the pubkey `HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye`, and
different names yield unrelated keys. Use distinct names for distinct roles.

These keys are public knowledge: anyone who knows the name can recompute the
secret key. They are meant for local networks only.

What is not determined by the script itself:

- the contents of files read with `keypair_file`, `file`, program binaries and
  IDLs;
- accounts fetched with `clone` and the feature set copied with `fork`, which
  reflect the remote cluster at the time of the run ([Genesis](genesis.md));
- runtime values such as `slot()`, `epoch()` and transaction signatures.

See also: [Concepts](../getting-started/concepts.md), [Networks](networks.md).
