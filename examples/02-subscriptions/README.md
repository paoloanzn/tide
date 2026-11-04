# Solana Subscriptions — a full-stack development *scaffold*

**Source:** [solana-foundation/subscriptions](https://github.com/solana-foundation/subscriptions/tree/0149b8c4fd362c1f02b1de6a4a5edfe14c724fba)  
**Pinned upstream commit:** `0149b8c4fd362c1f02b1de6a4a5edfe14c724fba`  
**License:** MIT (upstream program)  
**Status:** Draft fixture; **not yet independently run**. **No subscription instruction is executed by this Tide file yet.**

This repository contains a real Pinocchio subscriptions program, TypeScript/Rust clients and a React webapp. Its original development flows use Surfpool runbooks and, for the webapp, `solana-test-validator`.

Our `network.tid` defines the first useful layer of an isolated local environment:

- Preload the upstream binary at canonical program ID `De1egAFMkMWZSN5rYXRj9CAdheBamobVNubTsi9avR44`.
- Fund a merchant and two users using stable, **local-only** seeded keypairs.
- Create a synthetic 6-decimal SPL mint and three token accounts.
- Check fixture balances, then execute a real SPL transfer as a smoke test.
- Expose an RPC endpoint for developing an off-chain client.

**Important:** This example currently does **not** create on-chain subscriptions, create merchant plans, or demonstrate complete compatibility with the upstream React app. A working subscription flow requires proper account creation, instruction layout and an integration test with the upstream SDK.

### Build the program

From *this* directory, with Rust, Solana/Agave CLI and `tide` installed:

```sh
git clone https://github.com/solana-foundation/subscriptions.git upstream
git -C upstream checkout 0149b8c4fd362c1f02b1de6a4a5edfe14c724fba
(cd upstream/program && cargo build-sbf)
test -f upstream/target/deploy/subscriptions_program.so
```

If the upstream project changes build conventions, use the pinned `just build-program` recipe and confirm the output path. The Tide script looks for `upstream/target/deploy/subscriptions_program.so` relative to itself.

### Run

From the Tide repository root:

```sh
tide check examples/02-subscriptions/network.tid
tide up examples/02-subscriptions/network.tid
# RPC: http://127.0.0.1:28899
tide down examples/02-subscriptions/network.tid
```

Expected outcome **to verify**: two scenarios pass and the local RPC responds while the network remains running.

To use the upstream webapp, its wallet and SDK must be pointed at the new RPC, and the appropriate on-chain Subscription Authority / plan accounts must be initialized by the application or an additional setup script. Do not treat this environment as an end-to-end Subscriptions demo yet.

### Inspect the environment in a browser

After running `tide up`, start a separate HTTP server from this example directory:

```sh
cd examples/02-subscriptions
python3 -m http.server 8000
# Open http://127.0.0.1:8000/demo.html
```

[demo.html](demo.html) is a small **read-only RPC dashboard**: it queries the real program account, the test mint and merchant/Alice/Bob balances. It does not use the upstream webapp, send transactions or exercise subscription-plan logic. The fixture addresses are derived from the documented Tide seed algorithm. Browser-to-RPC CORS support depends on the validator configuration. This client has not yet been browser-tested in this branch.

### Agent prompt

> Read the pinned source for Solana Foundation Subscriptions and the Tide `network.tid` scaffold. Extend it with the minimum authentic instruction sequence to create a merchant subscription plan and a subscriber delegation. Prefer exact instruction layouts from the generated Codama SDK. Execute the resulting flows and assert account state; do not claim success unless the actual program executes on the local validator.

### Current comparison boundary

- Original Surfpool [setup runbook](https://github.com/solana-foundation/subscriptions/blob/0149b8c4fd362c1f02b1de6a4a5edfe14c724fba/runbooks/surfnet-setup/main.tx).
- Original [development tasks](https://github.com/solana-foundation/subscriptions/blob/0149b8c4fd362c1f02b1de6a4a5edfe14c724fba/justfile).
- Tide [network scaffold](network.tid).

The Tide script currently covers **program preload + fixture accounts + token-transfer smoke test**, not the complete original application's setup. Its simplicity should not be represented as a complete reduction of the existing workflow.
