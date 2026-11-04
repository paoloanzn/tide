# Real Solana environments, written in Tide

This directory holds **upstream-grounded examples** of running Solana programs in local networks created with Tide. Each example links to real upstream code, records an upstream commit and states what is—and is not—covered.

> Verification as of 2026-11-04: **`tide check` passed for both new scripts on Linux and macOS** in [GitHub Actions](https://github.com/paoloanzn/tide/actions/runs/41859558611). Neither example has yet been validated with its compiled upstream program by a clean-machine `tide up` run. Do not publish end-to-end pass claims until they are recorded.

| Example | Protocol/source | What it exercises | Current verification |
| --- | --- | --- | --- |
| [01 — Simple PDA](01-simple-pda/) | [txtx/surfpool-examples](https://github.com/txtx/surfpool-examples/tree/c783f4192aaebf48d39ea2787c51278527056982/anchor/simple-pda) | Preloaded Anchor program, deterministic PDA, real `invoke`, state decoding | Static check passed; runtime pending |
| [02 — Subscriptions](02-subscriptions/) | [solana-foundation/subscriptions](https://github.com/solana-foundation/subscriptions/tree/0149b8c4fd362c1f02b1de6a4a5edfe14c724fba) | Canonical program ID, deterministic users, synthetic SPL mint, live token transfer | **Scaffold only**; static check passed; subscription flows not implemented |
| [03 — Tokenized Vaults](03-vaults/) | Private application case study | Multi-program DeFi setup, external lending dependencies, stateful end-to-end scenarios | Author-reported successful run; no public executable source |

The [Subscriptions read-only dashboard](02-subscriptions/demo.html) is a lightweight frontend that queries the Tide-launched local RPC for the seeded wallets, mint and program account. It is **not** a full Subscriptions UI or payment-flow integration.

## What to measure

Judge the **whole environment**, not the length of one runbook. Keep a record of:
1. Upstream revision and required build artifacts.
2. Developer actions from clean checkout to working RPC.
3. Initial accounts and programs available before the first user action.
4. Real on-chain instructions successfully executed.
5. Errors and diagnostics when the environment is invalid.
6. Reproducibility limits (especially data fetched live from an upstream RPC).

For any comparison with Surfpool, use equivalent program binaries, account states, transactions and assertions. Surfpool may have capabilities that Tide v0.1.0 lacks, including lazy cloning and time travel. A shorter example does not establish parity.

## Reproducibility and security

`seed("name")` generates a **publicly derivable keypair**. Never transfer real funds to an example wallet. Dependencies under `upstream/` are intentionally not vendored. Follow each README's pinned checkout and build instructions.

Tide currently uses Agave's `solana-test-validator`. `tide check` checks syntax/types without a validator; `tide up` needs the validator and program binaries. Genesis account cloning from remote clusters is **not pinned to historical state** in v0.1.0.

## Next additions

- Add automated clean-machine end-to-end runs for examples 01 and 02.
- Complete program-specific Subscriptions initialization and subscription-plan flows.
- Create a fully redistributable DeFi integration sample whose program sources are publicly licensed.
- Compare a like-for-like Surfpool runbook and Tide environment with observed test results.
