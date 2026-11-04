# Tokenized Vaults — private DeFi case study

**Status:** Reported working by the project author; **not independently reproduced** from this public repository.  
**Public artifacts:** Architectural description only. The full program binaries, original script and private project source are **not redistributed**.

This is the real application scenario that motivated Tide: a tokenized vault protocol with custom Solana programs, external strategy programs, and an off-chain indexer, backend, keeper and frontend. Its contracts were not deployed to Devnet while the application was being developed.

The project author used a Tide script of roughly **640 lines** to prepare and exercise:

- Three locally compiled programs: vault core, Jupiter Lend adapter and custody adapter.
- Jupiter Lend program dependencies and market accounts sourced from Mainnet.
- Two vault configurations: an open USDC lending strategy and a permissioned treasury/custody strategy.
- Deterministically seeded local wallets for owners, managers, guardians, keepers, depositors and other roles.
- An initial state prepared in `genesis`, contract initialization transactions in `bootstrap` and **15 ordered scenarios** exercising deposits, allocations, authorization failures, keeper actions and withdrawals.

## What is interesting

The environment exists **before the upstream contracts are deployed publicly**, so an off-chain service can connect to the same local RPC used by the contract development workflow. The script captures both the intended topology and a concrete test narrative.

A notable step is importing the upgradeable Jupiter program data and adapting loader metadata to a fresh local ledger. This is an advanced integration-specific workaround, **not** a general guarantee of production fidelity.

## Reproducibility boundaries

- The author's original script works in their local setup; a clean-machine run has not been published here.
- Live Mainnet clones are **time-dependent**: Tide v0.1.0 does not pin account state by historical slot.
- Exact reproduction requires the local program source and binaries, versioned RPC fixtures, and any relevant off-chain test signatures.
- Sequential `scenario` blocks share state; snapshot/restore is not available in Tide v0.1.0.

## Public demo next step

Publish a sanitized, fully licensed version using independently redistributable programs and pinned fixtures, then record a fresh `tide up` run and protocol assertions. Until then, this is a **case study, not an install-and-run example**.

An example should never imply partnership, authorization or endorsement from Jupiter Lend or any third-party program.
