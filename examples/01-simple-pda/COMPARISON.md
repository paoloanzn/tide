# Simple PDA: read the workflow, not just the line count

This comparison uses a real upstream Anchor sample pinned to commit `c783f4192aaebf48d39ea2787c51278527056982`.

| Concern | Original Surfpool example | Tide port |
| --- | --- | --- |
| Program source | Same upstream Anchor program | Same upstream Anchor program |
| Program ID | `Akbajztf6ZSGXytTXKDd7MsGVviNCaEumvigp5BWaV8a` | Same ID |
| Program build | External Anchor build | External Anchor build |
| Installation | txtx [deployment runbook](https://github.com/txtx/surfpool-examples/blob/c783f4192aaebf48d39ea2787c51278527056982/anchor/simple-pda/runbooks/deployment/main.tx) | `genesis { program ... }` with preloaded binary |
| Caller | txtx [signer configuration](https://github.com/txtx/surfpool-examples/blob/c783f4192aaebf48d39ea2787c51278527056982/anchor/simple-pda/runbooks/call/signers.localnet.tx) | Local deterministic `seed("tide:simple-pda:caller")` |
| Instruction | txtx [`svm::process_instructions`](https://github.com/txtx/surfpool-examples/blob/c783f4192aaebf48d39ea2787c51278527056982/anchor/simple-pda/runbooks/call/main.tx) | `invoke PROGRAM with ...` |
| Expected state | Counter PDA created and incremented | Counter PDA created in bootstrap and checked in two scenarios |
| Verification | Upstream runbook returns a signature | Tide asserts decoded counter values |
| Simulator | Surfpool (LiteSVM-based) | Agave `solana-test-validator` |

**This is a comparison of programming models, not a performance benchmark or identical deployment methods.** Tide presently lacks Surfpool's lazy state loading and other simulator capabilities.

## Review questions for developers

- Can you see the intended network state and the execution steps without following links to multiple setup files?
- How many decisions are required to change the starting account state?
- What is the error experience when a PDA seed, signer or instruction argument is wrong?
- Can an AI agent modify the setup without changing unrelated deployment configuration?

## Required proof before announcing parity

Run both implementations using the pinned upstream source and report:
- Exact Surfpool, Tide, Anchor and Solana/Agave versions.
- Commit and hash of the compiled `simple_pda.so`.
- Whether the instruction succeeds in both environments.
- PDA address and decoded counter after the first and second invocation.
- The commands and manual interventions required from clean checkout.

A passing Tide `check` only establishes static validation, **not** proof that the binary executed successfully.
