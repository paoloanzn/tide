# Expressions and statements

Tide's expressions and statements are a small, conventional core shared by
every block that contains code: function bodies, `genesis`, `bootstrap` and
`scenario`. This page describes that core. The statements that only make sense
in one kind of block (`account` in genesis, `transaction` at runtime, and so on)
are described in [Genesis](genesis.md) and [Runtime](runtime.md); which
statement is allowed where is summarized in
[Networks](networks.md).

The types referred to here are described in [Types and values](types-and-values.md).

## Operators and precedence

Operators are listed from the tightest binding to the loosest:

| Level | Operators | Associativity | Example |
|---|---|---|---|
| 1 | `x.name`, `x[i]`, `f(args)` | left to right | `kp.pubkey`, `xs[0]`, `pda(p, ["s"])` |
| 2 | `-x`, `!x` (prefix) | right to left | `-amount`, `!ok` |
| 3 | `*`, `/`, `%` | left | `a * b / c` |
| 4 | `+`, `-` | left | `a + b - c` |
| 5 | `..`, `..=` | none (cannot be chained) | `0..n + 1` |
| 6 | `<`, `<=`, `>`, `>=` | left | `a < b` |
| 7 | `==`, `!=` | left | `a == b` |
| 8 | `&&` | left | `a && b` |
| 9 | `\|\|` | left | `a \|\| b` |

Parentheses override precedence. Some consequences worth knowing:

- `-x.y` is `-(x.y)`, and `!a == b` is `(!a) == b`.
- `0..n + 1` is `0..(n + 1)`, because ranges bind more loosely than `+`.
- `a && b || c` is `(a && b) || c`.
- `1..2..3` is a syntax error (`operator cannot be chained`).
- `1 < 2 < 3` parses as `(1 < 2) < 3` and is then rejected by the type checker
  (`operator '<' cannot be applied to bool and int`). Write `1 < x && x < 3`.

## Operator types

| Operator | Operands | Result |
|---|---|---|
| `+`, `-`, `*`, `/`, `%` | `int` and `int` | `int` (checked, see [Integers](types-and-values.md#integers)) |
| `+`, `-`, `*`, `/`, `%` | numbers, at least one `float` | `float` |
| `+`, `-` | `duration` and `duration` | `duration` |
| `*` | `duration` and `int`, either order | `duration` |
| `/` | `duration` and `int` | `duration` |
| `+` | `string` and `string` | `string` (concatenation) |
| `+` | `bytes` and `bytes` | `bytes` (concatenation) |
| `+` | `[T]` and `[U]` with a common element type | list (concatenation) |
| `-x` | `int`, `float` or `duration` | same type (`int` for any integer width) |
| `!x` | `bool` | `bool` |
| `<`, `<=`, `>`, `>=` | two numbers, two durations, or two strings | `bool` |
| `==`, `!=` | two values of compatible types, not keypairs | `bool` |
| `&&`, `\|\|` | `bool` and `bool` | `bool` |
| `..`, `..=` | `int` and `int` | `range` |

There are no implicit conversions to `string` and no truthiness:

```tide
let n = 3;
let label = "count: " + n;
```

```text
concat.tid:2:23: error: operator '+' cannot be applied to string and int
 2 | let label = "count: " + n;
   |                       ^
```

Use `"count: " + str(n)` instead.

Arithmetic errors that depend on values (overflow, division or modulo by zero)
are runtime errors. `tide check` does not evaluate expressions, so even
`1 / 0` passes the checker and fails when the code runs.

### Comparison and equality

`==` and `!=` compare structurally (lists element by element, objects field by
field) and accept any pair of types where one converts to the other, or two
numbers. An `int` and a `float` compare by numeric value. `Option` values are
compared with `null` this way (`info != null`). Keypairs cannot be compared;
compare their `.pubkey` members.

### Short-circuit evaluation

`&&` evaluates its right operand only when the left one is `true`; `||` only
when the left one is `false`. The right operand can therefore depend on a check
made by the left one:

```tide
network demo {
  genesis {
    let info = account_info(seed("maybe").pubkey);
    let funded = info != null && info.lamports > 0;
  }
}
```

## Member access, indexing and calls

- `x.name` reads a member: `.pubkey` on a keypair, `.url` on a source, or a
  field of an object. Through an `Option`, the access fails at runtime if the
  value is `null`. With an [import](declarations-and-scope.md#imports) alias,
  `lib.name` refers to a top-level name of that module.
- `x[i]` indexes a list or a `bytes` value with an `int`. Strings cannot be
  indexed. An out-of-range index is a runtime error.
- `f(a, b)` calls a user function or a builtin. Arguments are evaluated left to
  right, then checked against the parameter types (with
  [implicit conversions](types-and-values.md#implicit-conversions) and range
  checks). The number of arguments must match. Trailing commas are allowed.

Functions are not values: a function or builtin name must always be followed
by a call.

```tide
fn add(a: int, b: int) -> int { return a + b; }
let x = add(1);
```

```text
arity.tid:2:9: error: function 'add' expects 2 argument(s), found 1
 2 | let x = add(1);
   |         ^~~
arity.tid:1:4: note: 'add' is declared here
 1 | fn add(a: int, b: int) -> int { return a + b; }
   |    ^~~
```

Builtins and their signatures are listed in the
[standard library reference](../reference/standard-library.md). Function
declarations are covered in
[Declarations and scope](declarations-and-scope.md#functions).

## Ranges

`a..b` (exclusive) and `a..=b` (inclusive) build a `range` value from two
`int`s. Ranges are used with `for`:

```tide
network demo {
  genesis {
    for i in 0..3 { print(i); }    // 0 1 2
    for i in 1..=3 { print(i); }   // 1 2 3
  }
}
```

A range whose end is not past its start (`5..1`) is empty.

## Statements

Every statement ends with `;` except the block statements (`if`, `for`,
`while`), which end with their closing `}`. A block `{ ... }` opens a new scope:
names declared inside it are not visible after it (see
[Declarations and scope](declarations-and-scope.md#scopes-and-shadowing)).

### `let`

`let name = expr;` declares a mutable variable. An optional annotation fixes its
type: `let name: T = expr;`. Without an annotation the variable takes the type
of the initializer.

```tide
let fee = 5000;                   // int
let limit: u32 = 200000;          // range-checked u32
let authority: Option<pubkey> = null;
let ratio: float = 1;             // int literal used as a float
```

An annotation is required when the initializer is `null` or an empty list
whose element type matters. Inside blocks, `keypair` and `source` declarations
are not allowed; they belong at top level or directly in a network.

### Assignment

`target = expr;` replaces the value of a mutable variable, of an object field,
or of a list or `bytes` element. Targets can be nested:

```tide
network demo {
  genesis {
    let cfg = { limits: [1, 2], name: "a" };
    cfg.limits[1] *= 5;
    cfg.name += "b";
    print(cfg);   // {limits: [1, 10], name: "ab"}
  }
}
```

The compound operators `+=`, `-=`, `*=`, `/=` and `%=` combine the current
value with the right-hand side using the corresponding operator and store the
result. The result must fit the target's type:

```tide
let s = "x";
network demo { genesis { let t = s; t += 1; } }
```

```text
compound.tid:2:42: error: compound assignment cannot combine string with int
 2 | network demo { genesis { let t = s; t += 1; } }
   |                                          ^
```

Only `let` variables can be assigned. Keypairs, sources, function parameters,
loop variables, transaction results and import aliases are immutable, and so
are top-level `let` variables (see
[Immutability](declarations-and-scope.md#immutability)). Assigning to a
sized integer variable range-checks the new value at runtime.

Assignment is a statement, not an expression: it cannot appear inside another
expression.

### `if`, `else if`, `else`

```tide
fn sign(x: int) -> int {
  if x > 0 {
    return 1;
  } else if x < 0 {
    return -1;
  } else {
    return 0;
  }
}
```

The condition must be a `bool`; there is no truthiness
(`type mismatch in if condition: expected bool, found int`). Braces are always
required. `else if` is shorthand for an `else` block containing a single `if`.

### `for`

`for name in expr { ... }` iterates over a `range` (the variable is an `int`)
or a list (the variable has the element type). Iterating anything else is an
error (`cannot iterate over string; expected a range or a list`).

- The loop variable is a new immutable binding in each iteration.
- The iterable is evaluated once, before the first iteration. Changing the list
  variable inside the body does not change what the loop visits:

```tide
network demo {
  genesis {
    let xs = [1, 2, 3];
    for x in xs {
      xs = xs + [x * 10];
    }
    print(xs);   // [1, 2, 3, 10, 20, 30]
  }
}
```

### `while`

`while cond { ... }` runs the body as long as the `bool` condition is true. The
condition is evaluated before each iteration.

### `break` and `continue`

`break;` leaves the innermost loop; `continue;` skips to its next iteration.
Both are only valid inside a `for` or `while` body; elsewhere they are an
error (`'break' outside of a loop`).

```tide
network demo {
  genesis {
    let i = 0;
    let odd = 0;
    while i < 10 {
      i += 1;
      if i % 2 == 0 { continue; }
      if i > 7 { break; }
      odd += 1;
    }
    assert i == 9 && odd == 4;
  }
}
```

### `assert`

`assert cond;` or `assert cond, "message";` stops the current block with an
error when the `bool` condition is false. The message must be a string literal.
`assert` works in function bodies, genesis, bootstrap and scenarios; it is not
a top-level declaration.

```tide
let total = 2 + 2;
network demo { genesis { assert total == 5, "arithmetic is broken"; } }
```

```text
assert.tid:2:26: error: assertion failed: arithmetic is broken
 2 | network demo { genesis { assert total == 5, "arithmetic is broken"; } }
   |                          ^
```

What a failed assertion does to the network depends on the block it is in
(genesis aborts the launch, a scenario is only marked as failed); see
[Runtime](runtime.md).

### Expression statements

An expression followed by `;` is evaluated for its effects. This is meant for
calls such as `print(...)`. Any other expression used as a statement is legal
but produces a warning, because its value is thrown away:

```text
unused.tid:1:26: warning: expression result is unused
 1 | network demo { genesis { 1 + 2; } }
   |                          ^
```

### `return`

`return expr;` and `return;` are only valid in function bodies; see
[Functions](declarations-and-scope.md#functions).

## Execution budget

Every Tide run has a budget of 10,000,000 execution steps. It exists so that a
loop that never ends (for example a `while` whose condition never becomes
false) fails with a clear message instead of hanging `tide up` forever.

One step is counted for each statement executed and for each loop iteration.
The budget covers the whole run: top-level declarations, configuration,
genesis, bootstrap and the scenarios share one counter. Expressions inside a
statement are not counted separately.

```tide
network demo {
  genesis {
    let i = 0;
    while true { i += 1; }
  }
}
```

```text
loop.tid:4:5: error: execution step limit exceeded (10000000 steps); is there an infinite loop?
 4 |     while true { i += 1; }
   |     ^
```

Recursion has a separate limit, the call depth; see
[Functions](declarations-and-scope.md#functions). An `await` statement waits on
wall-clock time rather than steps; see [Runtime](runtime.md).

Next: [Declarations and scope](declarations-and-scope.md).
