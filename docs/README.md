# Tide documentation

This is the documentation for Tide v0.1.0, a language for describing local
Solana networks as code. If you have not used Tide before, read the
[project README](../README.md) first. It shows a complete script and how a
run works in a few minutes.

The pages build on each other in the order listed below. Each one assumes
you know the pages before it, and links back when it relies on an earlier
idea.

## Getting started

Start here if you are new to Tide.

| Page | What you will learn |
|---|---|
| [Concepts](getting-started/concepts.md) | The mental model: what a network definition is, why it is split into state and actions, and what is checked before anything runs. |
| [Tutorial](getting-started/tutorial.md) | How to build and run a small network one step at a time, including what failures look like. |
| [AI agent setup](../README.md#quick-start-with-an-ai-agent) | Install the official skill and let your agent check prerequisites and propose a network. |

## Language reference

These pages describe the language precisely. The first four cover the general
language: values, expressions, functions and modules. The last four cover the
parts that are specific to Solana networks.

| Page | Covers |
|---|---|
| [Lexical structure](language/lexical-structure.md) | Comments, identifiers, keywords, number literals and units, strings. |
| [Types and values](language/types-and-values.md) | Every type, implicit conversions, `Option`, lists, objects and copy semantics. |
| [Expressions and statements](language/expressions-and-statements.md) | Operators and precedence, control flow, assignment, `assert`. |
| [Declarations and scope](language/declarations-and-scope.md) | Top-level declarations, functions, imports, scoping and evaluation order. |
| [Networks](language/networks.md) | The `network` block, `options`, `validator`, and which statements each block allows. |
| [Genesis](language/genesis.md) | Declaring the initial state: accounts, programs, mints, token accounts, lookup tables, sysvars, features, forks and clones. |
| [Runtime](language/runtime.md) | `bootstrap` and `scenario` blocks: transactions, Anchor `invoke`, `deploy`, `airdrop`, `await`. |
| [Schemas](language/schemas.md) | Binary layouts for account and instruction data with Borsh and Anchor. |

## Reference

Look things up here once you know the language.

| Page | Covers |
|---|---|
| [Standard library](reference/standard-library.md) | Every builtin function: signature, result, errors and where it may be used. |
| [Command line](reference/cli.md) | `tide check`, `tide up`, `tide down`, their options, exit codes and the diagnostics format. |

## Guides

| Page | Covers |
|---|---|
| [Lifecycle](guides/lifecycle.md) | What `tide up` does in each phase, the state directory, and what happens when something fails. |

## Backends

| Page | Covers |
|---|---|
| [solana-test-validator](backends/solana-test-validator.md) | How Tide maps a network onto `solana-test-validator`, and the limitations of this backend. |

## Contributing

| Page | Covers |
|---|---|
| [Architecture](contributing/architecture.md) | Building from source, the code layout, the pipeline stages, the backend boundary and the tests. |

## Conventions used in these pages

- Code blocks tagged `tide` contain Tide source. Tide files use the `.tid`
  extension. The `tide-syntax` package provides highlighting for VS Code, Vim
  and Neovim.
- Every Tide example has been checked with `tide check`. Diagnostics and
  command output are copied from real runs, with file paths shortened.
- `tide` is assumed to be on your `PATH`.
