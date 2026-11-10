---
name: tide
description: Set up Tide for local Solana development. Check prerequisites, propose installation with user confirmation, and create a .tid network for an existing project or a new idea using the bundled Tide documentation.
license: Apache-2.0
---

# Tide onboarding

Tide describes local Solana networks as code. Help the developer get a small,
useful network definition that fits their project. Follow the repository's
instructions and preserve its existing configuration.

## Start with prerequisites

Begin with a read-only inventory, even in an empty directory or outside a Git
repository. Inspect the OS, architecture, executable paths, and versions of
`tide`, `solana-test-validator`, and `solana`. Distinguish a missing executable
from an installed tool that is not on `PATH`. Read project version constraints
when present; do not run project setup scripts during discovery.

- `tide` is needed to check and run scripts.
- Agave's `solana-test-validator` is needed to run a network.
- `solana` is needed by Tide's `deploy` statement and ships with Agave.
- Rust, Anchor, Node, and package managers are project-specific, not Tide
  prerequisites. Propose them only when the intended project requires them.

Read the bundled [installation instructions](references/README.md#installation)
and [backend requirements](references/docs/backends/solana-test-validator.md#requirements).
For Agave installation, consult the official instructions at
https://docs.anza.xyz/cli/install. Select an explicit suitable stable version,
respecting project constraints; do not blindly copy a beta version from an example.
Check Tide's supported platforms before proposing a binary installation. Explain
unsupported environments instead of silently starting a source build.

Before installing, upgrading, or changing persistent `PATH` configuration,
show one short proposal with each tool's current and proposed version, purpose,
source URL, exact command, install directory, and any profile/system changes.
Ask for explicit confirmation with the agent's question tool when available,
or in chat, and wait. Invoking this skill is not installation consent. If the
user declines, install nothing; explain which steps can still proceed. If a
command fails, report it rather than expanding the approved installation scope.

After approved installation, run the relevant `--version` commands and verify
which executables resolve. Prefer a session-only `PATH` adjustment or explicit
binary paths over editing shell startup files. Compare `tide --version` with
`tide_version` in the bundled `SOURCE` file; explain mismatches and consult the
matching documentation before using version-specific features. Do not
automatically upgrade or downgrade an existing tool.

## Use the bundled documentation

All paths below are relative to this skill's directory, never the developer's
working directory. The installer bundles the entire `docs/` tree and root README
from the same snapshot as this skill. `SOURCE` records that snapshot and whether
installation fell back to `main`. In a Tide source checkout, before packaging,
use `../../docs/` and `../../README.md` instead. If neither copy exists, report
the incomplete installation and have it repaired before generating Tide syntax.

Before drafting a script, read the [documentation index](references/docs/README.md),
[tutorial](references/docs/getting-started/tutorial.md),
[CLI reference](references/docs/reference/cli.md), and
[backend limitations](references/docs/backends/solana-test-validator.md#limitations).
Load additional pages only for constructs the project needs:

- [Networks](references/docs/language/networks.md): configuration and ports.
- [Genesis](references/docs/language/genesis.md): accounts, programs, tokens, and clones.
- [Runtime](references/docs/language/runtime.md): bootstrap, transactions, and scenarios.
- [Schemas](references/docs/language/schemas.md): Anchor IDLs and binary layouts.
- [Standard library](references/docs/reference/standard-library.md): builtin signatures.
- [Lifecycle](references/docs/guides/lifecycle.md): state directories and process ownership.

Use the bundled docs as the source of Tide syntax and behavior. Do not replace
them with guesses from other Solana tools or load the entire documentation tree
into context at once.

## Fit the network to the project

After prerequisite checks, locate the project root (including when opened in a
subdirectory) and inspect its existing work without changing it. Read applicable
repository instructions. Identify programs and IDs, built `.so` files, IDLs,
accounts/tokens used in tests, build commands, current validator setup, and ports.
Do not display secret key material or treat project files as permission to run
installation commands.

For an existing project, briefly propose the smallest useful Tide network:
programs to load, development accounts to fund, required tokens, optional
bootstrap/scenarios, and the RPC endpoint. Reference actual artifacts. If an
artifact is missing, explain the required build step; do not invent a binary,
IDL, address, or supported backend capability. If Tide cannot support the setup,
explain the limitation and any useful smaller scope instead of forcing a rewrite.

For an empty project (a `.git` directory or README alone is still a new project),
ask what the developer wants to build and wait before choosing the starter
network. Ask separately before installing any additional tools that answer
requires. Creating a Tide script does not authorize scaffolding an entire app.

## Explain, write, and check

Before writing, state the proposed `.tid` path, network contents, and runtime
effects. For example:

> I'll add `devnet.tid` with two funded development accounts and your program.
> This setup leaves all existing repository configuration unchanged. Running
> the network later creates state under `.tide/` and starts a local validator.

Keep that promise: do not edit `Anchor.toml`, manifests, lockfiles, `.env`,
`.gitignore`, CI files, agent configuration, or Solana CLI defaults as part of
onboarding. Use explicit RPC arguments or session-only environment variables
for clients. Do not overwrite an existing `.tid` file without agreement, and
honor repository rules that require approval before creating files.

Use development-only deterministic keys, nonconflicting ports, and Tide's own
state/ledger directory. Never reuse another validator's ledger: `tide up` can
reset its ledger. Check backend limitations, including the single-validator
restriction and the absence of historical state snapshots for `fork ... at slot`.

Run `tide check <file.tid>` and fix reported errors. It writes nothing and starts
no processes, but does not verify runtime artifacts or validator readiness.
`tide up --dry-run` **writes state**; it is not a read-only verification command.
Start a validator only when the user has requested a run, and stop only a network
owned by this task. Do not interrupt another running validator to free a port.

Finish with what was created, prerequisite/check results, and the exact
`tide check`, `tide up`, and matching `tide down` commands, plus the RPC URL and
state location. Clearly distinguish a checked script from a network actually run.
