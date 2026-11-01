# Lexical structure

This page covers how Tide source text is split into tokens: comments,
whitespace, identifiers, keywords and literals. The meaning of the values those
literals produce is covered in [Types and values](types-and-values.md).

Tide source files use the `.tid` extension and are read as UTF-8.

## Comments and whitespace

Tide has two comment forms:

- `// ...` runs to the end of the line.
- `/* ... */` runs to the first `*/` and may span lines.

```tide
// A line comment.
/* A block comment
   spanning two lines. */
let fee = 5000; // Comments can follow code.
```

Comments and whitespace (spaces, tabs, newlines) can appear between any two
tokens and are otherwise ignored. Newlines carry no meaning: statements end with
`;`, not with a line break.

Block comments do not nest. The first `*/` closes the comment, so the remainder
of an outer comment becomes code:

```tide
/* outer /* inner */ still outside */
let x = 1;
```

```text
nest.tid:1:22: error: expected a declaration (import, source, keypair, let, schema, fn or network)
 1 | /* outer /* inner */ still outside */
   |                      ^
```

A block comment that is never closed is reported at the end of the file
(`expected '*/' but reached end of file`).

## Identifiers

An identifier starts with an ASCII letter or `_`, followed by any number of
ASCII letters, digits or `_`. Identifiers are case-sensitive.

```tide
let alice_balance = 1;
let _scratch2 = 2;
let Alice_balance = 3; // a different name from alice_balance
```

Non-ASCII letters are not accepted in identifiers (they are accepted inside
string literals). An identifier cannot start with a digit:

```text
digit.tid:1:5: error: expected an identifier
 1 | let 2x = 1;
   |     ^
```

## Keywords

Tide has two kinds of keywords. The split exists so that common words such as
`from`, `amount` or `slot` stay available as variable names while still working
as keywords inside the statements that need them.

### Reserved keywords

Reserved keywords can never be used as identifiers (variable, function,
parameter, schema, network, scenario or validator names, or import aliases):

```text
import  as  source  keypair  let  schema  fn  network  options  validator
genesis  bootstrap  scenario  if  else  for  in  while  break  continue
return  true  false  null  transaction  instruction  invoke  deploy  airdrop
warp  snapshot  restore  assert  await  account  program  mint  token_account
associated_token_account  lookup_table  sysvar  feature  fork  clone  delete
extension
```

`warp`, `snapshot` and `restore` are reserved for a future version. They parse,
but the checker always rejects them in v0.1.0.

Using a reserved word as a name is a syntax error:

```tide
let mint = 1;
```

```text
reserved.tid:1:5: error: 'mint' is a reserved keyword and cannot be used as an identifier
 1 | let mint = 1;
   |     ^~~~
```

### Contextual keywords

Contextual keywords have a special meaning only at the position where the
grammar expects them. Everywhere else they are ordinary identifiers:

| Word | Where it is a keyword |
|---|---|
| `using` | `schema Name using borsh { ... }` |
| `from` | `fork from`, `clone ... from`, `deploy ... from` |
| `with` | `invoke <program> with { ... }` |
| `at`, `slot` | `at slot N` in `fork` and `clone` |
| `amount` | `airdrop <pubkey> amount <lamports>` |
| `enabled`, `disabled` | `feature <id> = enabled;` |
| `to`, `by`, `slots`, `epoch`, `time` | the reserved `warp` statement |
| `SOL`, `lamports`, `ms`, `s`, `slots` | unit suffixes after a number literal (see [Units](#unit-suffixes)) |

```tide
let from = 1;
let amount = from + 1;
let s = 3;
let slots = 4;
```

This is valid. Note one consequence: a unit word directly after a number
literal is always read as a unit, so `2 s` is the duration two seconds, never
`2` followed by the variable `s`.

### Reserved words as keys

Record keys, entity field names, member names and object-literal keys accept
any identifier-shaped word, including reserved keywords. This is why a token
account can have a `mint` field even though `mint` is reserved:

```tide
let o = { mint: 1, account: 2 };
let total = o.mint + o.account;
```

The same applies to `key = value;` entries inside blocks such as
`token_account` (`mint = usd.pubkey;`), which are covered in
[Genesis](genesis.md).

## Number literals

A number literal is a sequence of decimal digits, optionally followed by a `.`
and more digits:

| Literal | Kind |
|---|---|
| `0`, `42`, `007` | integer (leading zeros are allowed and ignored) |
| `1.5`, `0.25`, `1.50` | float |

There is no other number syntax:

- no sign in the literal: `-5` is the unary minus operator applied to `5`
  (see [Expressions and statements](expressions-and-statements.md#operators-and-precedence));
- no digit separators: `1_000` is a syntax error;
- no hexadecimal, octal or binary forms (use `hex("...")` for byte strings);
- no exponent: `1e9` is a syntax error;
- a fractional part needs digits on both sides of the `.`: `.5` is a syntax
  error, and in `1.` the `.` is read as member access, which then expects a name.

Because a `.` only starts a fraction when a digit follows it, `1..3` is the
range `1` to `3`, not a malformed float.

Integer literals must fit in a signed 128-bit integer; a larger literal is a
check-time error (`integer literal is too large`). A literal without a decimal
point is a `float` when the context expects one, so `let f: float = 1;` is
accepted.

## Unit suffixes

A number literal may be followed by a unit. The unit is folded into the value
at check time:

| Suffix | Result type | Value stored |
|---|---|---|
| `lamports` | `int` | the number as is |
| `SOL` | `int` | the number times 1 000 000 000 (lamports) |
| `slots` | `int` | the number as is |
| `ms` | `duration` | milliseconds |
| `s` | `duration` | the number times 1000 (milliseconds) |

```tide
let rent = 2039280 lamports;
let stake = 1.5 SOL;   // 1500000000
let wait = 250 ms;
let timeout = 1.5 s;   // 1500 milliseconds
let window = 32 slots; // 32
```

Whitespace between the number and the unit is optional (`1SOL` and `5s` are
accepted). Units apply only to literals: there is no way to attach a unit to a
variable. To convert a computed SOL amount, use the builtin `sol(n)` (see the
[standard library](../reference/standard-library.md)).

The rules for fractional values (for example `0.0000000001 SOL` is an error)
are described in [Types and values](types-and-values.md#units-and-check-time-folding).

## String literals

A string literal is enclosed in double quotes and must fit on one line. It
supports the JSON escape sequences:

| Escape | Character |
|---|---|
| `\"` | double quote |
| `\\` | backslash |
| `\/` | forward slash |
| `\b`, `\f`, `\n`, `\r`, `\t` | backspace, form feed, newline, carriage return, tab |
| `\uXXXX` | the Unicode code point with that 4-digit hexadecimal value |

```tide
let greeting = "café \"quoted\"\n";
let path = "./target/deploy/counter.so";
```

Raw control characters (including a literal tab) are not allowed inside the
quotes; write the escape instead. Other UTF-8 text is allowed as is.

Common mistakes:

```text
str.tid:1:10: error: unterminated string literal
 1 | let s = "abc
   |          ^~~
```

```text
esc.tid:1:11: error: invalid escape sequence in string literal
 1 | let s = "a\qb";
   |           ^
```

## Booleans and null

`true` and `false` are the two `bool` literals. `null` is the absence of a
value; it is only meaningful where an `Option<T>` is expected (see
[Option](types-and-values.md#optiont-and-null)). All three are reserved
keywords.

## Punctuation

For reference, these are the operator and punctuation tokens of the language:

```text
+  -  *  /  %  !  ==  !=  <  <=  >  >=  &&  ||  ..  ..=
=  +=  -=  *=  /=  %=  ->  .  ,  ;  :  ( )  [ ]  { }  < >
```

Their meaning and precedence are described in
[Expressions and statements](expressions-and-statements.md).

Next: [Types and values](types-and-values.md).
