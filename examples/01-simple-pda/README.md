# Simple PDA — a port of an official Surfpool example

**Source:** [txtx/surfpool-examples / anchor/simple-pda](https://github.com/txtx/surfpool-examples/tree/c783f4192aaebf48d39ea2787c51278527056982/anchor/simple-pda)  
**Pinned upstream commit:** `c783f4192aaebf48d39ea2787c51278527056982`  
**Status:** Tide script and minimal instruction IDL authored; **not yet end-to-end verified** on this branch.

The upstream sample uses Surfpool/txtx deployment and call runbooks to deploy an Anchor program whose `run` instruction creates (if needed) a PDA and increments an on-chain counter.

Our Tide script uses the **same program ID** (`Akbajztf6ZSGXytTXKDd7MsGVviNCaEumvigp5BWaV8a`) and the same `run` instruction. It deliberately goes slightly beyond the original runbook by checking the decoded state.

### Read the two representations

- Upstream [deploy runbook](https://github.com/txtx/surfpool-examples/blob/c783f4192aaebf48d39ea2787c51278527056982/anchor/simple-pda/runbooks/deployment/main.tx)
- Upstream [call runbook](https://github.com/txtx/surfpool-examples/blob/c783f4192aaebf48d39ea2787c51278527056982/anchor/simple-pda/runbooks/call/main.tx)
- Tide [network.tid](network.tid)

**Comparison boundary:** The upstream runbooks and this Tide file use different deployment mechanisms: Surfpool's runbook deploys a program; Tide preloads the compiled binary into genesis. They exercise the same instruction and PDA semantics, but deployment performance and mechanism are **not equivalent**.

### Build upstream

Requires Solana/Agave CLI with `cargo build-sbf`, Rust and an installed `tide` binary.

From *this* directory:

```sh
git clone https://github.com/txtx/surfpool-examples.git upstream
git -C upstream checkout c783f4192aaebf48d39ea2787c51278527056982
(
  cd upstream/anchor/simple-pda
  anchor build
)
test -f upstream/anchor/simple-pda/target/deploy/simple_pda.so
```

`anchor build` requires the Anchor toolchain matching the upstream project. If its version is incompatible with your installed toolchain, consult the pinned upstream repository rather than silently changing the source revision.

The checked-in `simple_pda.idl.json` is a **minimal locally authored instruction interface**, based on the upstream Rust program (`run`, its three accounts and canonical Anchor discriminator). It is not claimed to be the complete official Anchor IDL.

### Check and launch

From the repository root:

```sh
tide check examples/01-simple-pda/network.tid
tide up examples/01-simple-pda/network.tid
# RPC: http://127.0.0.1:18899
tide down examples/01-simple-pda/network.tid
```

Expected behavior **to verify**:
- Genesis preloads the compiled `simple_pda.so`.
- Bootstrap invokes `run` once, creating the `custom` PDA with counter = 1.
- `initialized` asserts 1; `increment` invokes again and asserts 2.
- The network remains running after `tide up` returns.

The calls depend on correct program binaries and validator behavior; the expected outcomes are **not test results**.

### Agent prompt

> Inspect this pinned upstream Solana program. Build its SBF binary, review `network.tid`, run `tide check` and `tide up`, and diagnose any errors without changing the program ID or the intended PDA seeds. Report the exact Tide, Agave and Anchor versions and whether both scenarios pass.

### Security and attribution

This example does not redistribute upstream binaries. The `caller` key is deterministically generated and **unsafe for real funds**. The upstream program and runbooks remain credited to their maintainers. Compare the whole workflow, including build prerequisites, rather than line counts.
