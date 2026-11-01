# Tide

Tide is a language for describing local Solana networks as code. A Tide
script states which accounts, programs and SPL tokens exist when the network
starts, how the validator is configured, and which transactions and checks to
run against it. The `tide` command reads the script, builds that state, and
launches a `solana-test-validator` from it.

This is version 0.1.0.

## Why Tide

A local Solana setup for development or testing is usually a mix of shell
scripts, validator flags, keypair files and one-off client code that funds
accounts and creates tokens. That setup is hard to read, hard to reproduce,
and errors surface late, often halfway through a run.

Tide replaces it with one declarative file:

- **Reproducible.** Keys can be derived from names (`seed("alice")`), so the
  same script produces the same addresses and the same state on every machine.
- **Checked before it runs.** Types, field names, required fields and
  addresses are validated up front. 
- **Solana-native.** Accounts, mints, token accounts, programs, PDAs and
  transactions are part of the language.

## How a run works

`tide up` takes a script through a fixed sequence of phases.

1. **Check.** The script is parsed and validated.
2. **Configure.** The `options` and `validator` blocks set validator flags, such as ports and epoch length.
3. **Genesis.** The `genesis` block is executed to build the initial state: accounts, programs, mints, token accounts. No validator is running yet. This phase only computes state.
4. **Bootstrap (optional).** A `bootstrap` block runs on a private, temporary validator. Use it for setup that needs real transactions, such as initialising a program. Its effects are kept and the temporary validator is stopped.
5. **Persist.** The resulting state is written to disk, under `.tide/<network>/` next to the script by default.
6. **Launch.** The network is started from the persisted state as a background process.
7. **Scenarios.** Each `scenario` block runs against the launched network. A failing scenario is reported, but the network and the remaining scenarios keep running.

Tide then exits and the network keeps running. `tide down` stops it.

If any phase up to and including launch fails, Tide stops anything it started
and no network is left running.

## Installation

<!-- TBD -->

## Quick Start

### 1. A network with two funded accounts

Save this as `hello.tid`:

```tide
// Keypairs derived from names: the same name always gives the same key.
keypair alice = seed("alice");
keypair bob = seed("bob");

network hello {
  // Run the validator on non-default ports.
  validator main {
    rpc_port = 38899;
    gossip_port = 38001;
    faucet_port = 39900;
    dynamic_port_range = "38002-38040";
  }

  // Genesis: accounts that exist from the very first slot.
  genesis {
    account alice.pubkey { lamports = 10 SOL; }
    account bob.pubkey { lamports = 1 SOL; }
  }
}
```

The file has two parts. The top level declares values, here two keypairs.
The `network` block describes one network: a `validator` block for its
configuration and a `genesis` block for its initial state. `10 SOL` is a
number with a unit; Tide converts it to lamports when it checks the script.
Fields you leave out of an `account`, such as `owner`, take defaults. Here the
owner is the system program.

Validate the script without running anything:

```sh
tide check hello.tid
```

```text
hello.tid: ok (1 network(s))
```

Mistakes are reported before anything starts. If `lamports` were misspelled
as `lamport`, the check would print:

```text
hello.tid:16:28: error: unknown field 'lamport' in account; did you mean 'lamports'?
 16 |     account alice.pubkey { lamport = 10 SOL; }
    |                            ^~~~~~~
hello.tid:16:5: error: missing required field 'lamports' in account
 16 |     account alice.pubkey { lamport = 10 SOL; }
    |     ^
2 error(s), 0 warning(s)
```

### 2. Tokens, a bootstrap transaction and a scenario

The next script adds an SPL token, moves some of it with a real transaction,
and checks the result on the running network. Save it as `quickstart.tid`:

```tide
keypair alice = seed("alice");
keypair bob = seed("bob");
keypair usd = seed("usd");                       // the mint's keypair
let alice_usd = ata(alice.pubkey, usd.pubkey);   // associated token addresses
let bob_usd = ata(bob.pubkey, usd.pubkey);

network quickstart {
  validator main {
    rpc_port = 38899;
    gossip_port = 38001;
    faucet_port = 39900;
    dynamic_port_range = "38002-38040";
  }

  genesis {
    account alice.pubkey { lamports = 10 SOL; }
    account bob.pubkey { lamports = 1 SOL; }

    // A mint with 6 decimals; all 1,000,000 base units start in Alice's account.
    mint usd.pubkey { decimals = 6; mint_authority = alice.pubkey; supply = 1000000; }
    associated_token_account alice_usd { mint = usd.pubkey; owner = alice.pubkey; amount = 1000000; }
    associated_token_account bob_usd { mint = usd.pubkey; owner = bob.pubkey; amount = 0; }
  }

  // Runs once on a private validator; its effects become part of the saved state.
  bootstrap {
    transaction as tx {
      signers = [alice];  // Alice signs and, as the first signer, pays the fee
      instruction token_program() {
        accounts = [alice_usd, bob_usd, alice.pubkey];  // source, destination, owner
        data = spl_transfer(250000);
      }
    }
    assert tx.success, "transfer failed";
  }

  // Runs against the launched network every time you run `tide up`.
  scenario balances {
    assert token_balance(alice_usd) == 750000;
    assert token_balance(bob_usd) == 250000;
  }
}
```

The script uses each kind of block once:

- **Top-level `let`** computes the associated token account addresses once,
  so the rest of the script can refer to them by name.
- **`genesis`** creates the mint and both token accounts directly. No
  transactions are involved; the accounts exist when the network starts.
- **`bootstrap`** sends a real SPL Token transfer. `transaction as tx` binds the
  result to `tx`, so a failure does not abort the block on its own; the next
  line checks `tx.success`.
- **`scenario balances`** runs after launch and checks the balances on the
  live network.

Start the network:

```sh
tide up quickstart.tid
```

```text
==> configuring network 'quickstart'
==> building genesis state
    5 genesis account(s) defined
==> running bootstrap
    starting solana-test-validator (bootstrap, rpc http://127.0.0.1:38899)
    transaction succeeded: 2Uodhu2KTjYHDhiHrc5tQA7wZ8X5xTLsDZ5nzFZeKpgTBCL8dLC5MrAV2pA1DVdHvcfJ6wkbMVMaPLF6igjkVT8y
==> persisting state
    waiting for bootstrap state to finalize (slot 2)
    stopping bootstrap instance
==> launching network
    starting solana-test-validator (network, rpc http://127.0.0.1:38899)
==> scenario 'balances'
    scenario 'balances' passed

network 'quickstart' is running
  rpc:   http://127.0.0.1:38899
  state: /home/you/project/.tide/quickstart
  pid:   224230
  stop:  tide down quickstart.tid
scenarios: 1 passed, 0 failed
```

The output follows the phases listed in [How a run works](#how-a-run-works).
The network is now available at `http://127.0.0.1:38899`, so any Solana
client or the `solana` CLI can connect to it. Stop it with:

```sh
tide down quickstart.tid
```

```text
stopping validator (pid 224230)
network 'quickstart' stopped
```

## Documentation

The full documentation is in [`docs/`](docs/README.md). Good places to start:

- [Concepts](docs/getting-started/concepts.md): the ideas behind Tide.
- [Tutorial](docs/getting-started/tutorial.md): build a network step by step.
- [Language reference](docs/README.md#language-reference): the language, page by page.
- [CLI reference](docs/reference/cli.md): commands, options and exit codes.
