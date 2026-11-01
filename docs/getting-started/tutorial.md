# Tutorial: build a token network

This tutorial builds one small network, `token_net`, in six steps. Each step
adds one idea to the same file, `token-net.tid`: validator configuration,
funded accounts, an SPL mint with token accounts, a bootstrap transaction,
scenarios, and teardown. At the end you have a network with two users, a
token, and a transfer already applied, plus checks that run against it.

Read [Concepts](concepts.md) first if you have not: it explains the
static/dynamic split and the lifecycle that this tutorial uses.

## Before you start

You need:

- `tide` on your `PATH`,
- Agave's `solana-test-validator` on your `PATH` (`tide up` starts it),
- an empty working directory.

The examples use RPC port `18899` and other non-default ports so the network
does not collide with a validator you may already run on `8899`. The output
below is from real runs. Process IDs, transaction signatures and the
absolute state path will differ on your machine; the state path is shortened
to `.../.tide/token_net` here.

## Step 1: an empty network

Start with a network that only configures the validator.

```tide
network token_net {
  options {
    slots_per_epoch = 64;
    confirm_timeout = 30 s;
  }
  validator primary {
    rpc_port = 18899;
    faucet_port = 19900;
    gossip_port = 18001;
    dynamic_port_range = "18002-18040";
  }
}
```

- `options` holds network-wide settings. `slots_per_epoch` is passed to the
  validator. `confirm_timeout` is how long Tide waits for a transaction to
  confirm or an `await` condition to become true (the default is 60 s).
- `validator primary` configures the validator's ports. The name `primary`
  is a label; the solana-test-validator backend runs exactly one validator.

Check the file, then start the network:

```sh
tide check token-net.tid
tide up token-net.tid
```

```text
token-net.tid: ok (1 network(s))
```

```text
==> configuring network 'token_net'
    0 genesis account(s) defined
==> persisting state
==> launching network
    starting solana-test-validator (network, rpc http://127.0.0.1:18899)

network 'token_net' is running
  rpc:   http://127.0.0.1:18899
  state: .../.tide/token_net
  pid:   221239
  stop:  tide down token-net.tid
```

`tide check` only parses and validates; it starts nothing. `tide up` ran the
lifecycle and exited, and the validator is still running in the background.
Any Solana tool can use it, for example
`solana --url http://127.0.0.1:18899 cluster-version`.

A network must be stopped before it can be started again. Running `tide up`
a second time now fails:

```text
==> configuring network 'token_net'
token-net.tid:1:9: error: network 'token_net' is already running (pid 221239); stop it first with 'tide down'
 1 | network token_net {
   |         ^~~~~~~~~
1 error(s), 0 warning(s)
```

So between steps, stop the network with `tide down token-net.tid` (covered
in [step 6](#step-6-stop-the-network)) and run `tide up` again after editing.

## Step 2: funded accounts

Add two users. Their keys come from `seed(...)`, so the addresses are the same
on every machine and every run.

```tide
keypair alice = seed("alice");
keypair bob = seed("bob");

network token_net {
  // options and validator as in step 1

  genesis {
    account alice.pubkey { lamports = 10 SOL; }
    account bob.pubkey { lamports = 2 SOL; }
    print("alice:", alice.pubkey);
    print("bob:  ", bob.pubkey);
    assert balance(alice.pubkey) == 10 SOL;
  }
}
```

- `keypair` declarations sit at the top level so every block can use them.
- `account <pubkey> { ... }` writes an account directly into the starting
  state. Only `lamports` is required; the owner defaults to the system
  program.
- `balance(...)` inside `genesis` reads the state being built, so the
  `assert` runs before any validator starts.

```text
==> configuring network 'token_net'
==> building genesis state
alice: HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye
bob:   CJHsd5oHaKftDqzDFiHpB3LqN82ftpdoYydM8qbEW6Zm
    2 genesis account(s) defined
==> persisting state
==> launching network
    starting solana-test-validator (network, rpc http://127.0.0.1:18899)

network 'token_net' is running
  rpc:   http://127.0.0.1:18899
  state: .../.tide/token_net
  pid:   221828
  stop:  tide down token-net.tid
```

You will get the same two addresses. Confirm the balance with the Solana CLI:

```sh
solana --url http://127.0.0.1:18899 balance HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye
```

```text
10 SOL
```

Remove the two `print` lines before continuing; they were only there to show
the addresses.

## Step 3: a mint and associated token accounts

Add an SPL token, `usd`, with 6 decimals, and an associated token account
(ATA) for each user. Alice holds the whole supply.

```tide
keypair alice = seed("alice");
keypair bob = seed("bob");
keypair usd = seed("usd");
let alice_usd = ata(alice.pubkey, usd.pubkey);
let bob_usd = ata(bob.pubkey, usd.pubkey);

network token_net {
  // options and validator as in step 1

  genesis {
    account alice.pubkey { lamports = 10 SOL; }
    account bob.pubkey { lamports = 2 SOL; }

    mint usd.pubkey { decimals = 6; mint_authority = alice.pubkey; supply = 1000000; }
    associated_token_account alice_usd { mint = usd.pubkey; owner = alice.pubkey; amount = 1000000; }
    associated_token_account bob_usd { mint = usd.pubkey; owner = bob.pubkey; amount = 0; }

    assert token_balance(alice_usd) == 1000000;
  }
}
```

- `ata(owner, mint)` computes the associated token address, the same address
  the ATA program derives. Binding it with `let` at the top level lets every
  block refer to `alice_usd` and `bob_usd`.
- `mint` and `associated_token_account` write SPL Token accounts directly,
  with the right owner, layout and rent-exempt lamports. No transactions are
  involved. The full field lists are in [genesis](../language/genesis.md).
- `supply` and `amount` are in base units: with 6 decimals, `1000000` is one
  token.

```text
==> configuring network 'token_net'
==> building genesis state
    5 genesis account(s) defined
==> persisting state
==> launching network
    starting solana-test-validator (network, rpc http://127.0.0.1:18899)

network 'token_net' is running
  rpc:   http://127.0.0.1:18899
  state: .../.tide/token_net
  pid:   222417
  stop:  tide down token-net.tid
```

The five accounts are alice, bob, the mint, and the two ATAs.

### A common mistake: swapped `ata` arguments

`ata` takes the owner first and the mint second. Swapping them still type
checks, because both arguments are pubkeys:

```tide
let bob_usd = ata(usd.pubkey, bob.pubkey);   // wrong order
```

```text
token-net.tid: ok (1 network(s))
```

`tide check` cannot know which pubkey is meant to be the owner. `tide up`
evaluates the address, compares it with the `owner` and `mint` fields of the
declaration, and stops before starting anything:

```text
==> configuring network 'token_net'
==> building genesis state
token-net.tid:25:30: error: address 8YPS9qcm62seeDuCuva33SVz7pM5wAkgygpBkoktSk79 is not the associated token account of owner CJHsd5oHaKftDqzDFiHpB3LqN82ftpdoYydM8qbEW6Zm for mint CCrDymVfTunhmzpxP8kD9QxQ5DbB8rkd2niZCZz3FEGi (expected 6QRJxFm6STQ9bn6w7kUGomwcqFUhwAMcXR7L7m4QNTFx)
 25 |     associated_token_account bob_usd { mint = usd.pubkey; owner = bob.pubkey; amount = 0; }
    |                              ^~~~~~~
1 error(s), 0 warning(s)
```

The exit code is `1` and no validator is left running. Mistakes that the
checker can see, such as a misspelled field, are reported by `tide check`
already:

```text
token-net.tid:21:26: error: unknown field 'lamport' in account; did you mean 'lamports'?
 21 |     account bob.pubkey { lamport = 2 SOL; }
    |                          ^~~~~~~
token-net.tid:21:5: error: missing required field 'lamports' in account
 21 |     account bob.pubkey { lamport = 2 SOL; }
    |     ^
2 error(s), 0 warning(s)
```

[Concepts](concepts.md#check-time-and-run-time-errors) lists what each stage
catches.

## Step 4: a bootstrap transaction

Genesis declares state directly. Some setup should go through real programs
instead, so that the result is exactly what the chain would produce. Add a
`bootstrap` block that transfers 0.25 tokens from alice to bob and attaches a
memo, in one transaction. Add it inside the network, after `genesis`:

```tide
  bootstrap {
    transaction as tx {
      signers = [alice];
      instruction token_program() {
        accounts = [alice_usd, bob_usd, alice.pubkey];
        data = spl_transfer(250000);
      }
      instruction memo_program() { accounts = []; data = memo("tide bootstrap"); }
    }
    assert tx.success, "transfer failed";
    print("bootstrap transfer:", tx.signature);
  }
```

- `transaction` builds, signs and sends a transaction. Each `instruction`
  names the program and lists its accounts and data. `spl_transfer(n)` and
  `memo(s)` produce the instruction data.
- Plain pubkeys in `accounts` are writable, and a pubkey that belongs to a
  signer (`alice.pubkey`) is marked as a signer. The fee payer defaults to
  the first signer.
- `as tx` binds the result. A bound transaction does not abort the block when
  it fails; you inspect `tx.success`, `tx.signature`, `tx.error`, `tx.logs`
  and `tx.slot` yourself. Here the `assert` turns a failure into an error.

```text
==> configuring network 'token_net'
==> building genesis state
    5 genesis account(s) defined
==> running bootstrap
    starting solana-test-validator (bootstrap, rpc http://127.0.0.1:18899)
    transaction succeeded: 2oTLX3okZpZweQuVue5y546QCuueZrV7RgDmvYeReBhtT24xepQTBhaU5f8yexSJdufJnhUy9eAChgPAZ8ZbJy8E
bootstrap transfer: 2oTLX3okZpZweQuVue5y546QCuueZrV7RgDmvYeReBhtT24xepQTBhaU5f8yexSJdufJnhUy9eAChgPAZ8ZbJy8E
==> persisting state
    waiting for bootstrap state to finalize (slot 2)
    stopping bootstrap instance
==> launching network
    starting solana-test-validator (network, rpc http://127.0.0.1:18899)

network 'token_net' is running
  rpc:   http://127.0.0.1:18899
  state: .../.tide/token_net
  pid:   224842
  stop:  tide down token-net.tid
```

The output shows the bootstrap sequence: Tide started a private validator
instance from the genesis state, ran the block, waited until the transfer was
finalized, stopped the instance, and launched the network from that ledger.
The transfer is part of the network's starting state.

## Step 5: scenarios

Scenarios run against the launched network. Add two inside the network,
after `bootstrap`. The first checks the balances left by the bootstrap
transfer. The second fails on purpose: bob signs a transfer out of alice's
token account, and the transaction is not bound with `as`.

```tide
  scenario balances {
    assert token_balance(bob_usd) == 250000;
    assert token_balance(alice_usd) == 750000;
  }

  // Deliberately failing: bob signs a transfer out of alice's account.
  scenario bob_spends_alice {
    transaction {
      signers = [bob];
      instruction token_program() {
        accounts = [alice_usd, bob_usd, bob.pubkey];
        data = spl_transfer(1);
      }
    }
    print("not reached");
  }
```

```sh
tide up token-net.tid
echo $?
```

```text
==> configuring network 'token_net'
==> building genesis state
    5 genesis account(s) defined
==> running bootstrap
    starting solana-test-validator (bootstrap, rpc http://127.0.0.1:18899)
    transaction succeeded: 4xUzkJcjX9KjVYZTMTNVyHNr3HZ4hQmNMghb9Wbes2DwgeEECiygM1NgchV5FkYRUhsTYRmJ4ckEpaeEYNestCeg
bootstrap transfer: 4xUzkJcjX9KjVYZTMTNVyHNr3HZ4hQmNMghb9Wbes2DwgeEECiygM1NgchV5FkYRUhsTYRmJ4ckEpaeEYNestCeg
==> persisting state
    waiting for bootstrap state to finalize (slot 2)
    stopping bootstrap instance
==> launching network
    starting solana-test-validator (network, rpc http://127.0.0.1:18899)
==> scenario 'balances'
    scenario 'balances' passed
==> scenario 'bob_spends_alice'
    scenario 'bob_spends_alice' FAILED

network 'token_net' is running
  rpc:   http://127.0.0.1:18899
  state: .../.tide/token_net
  pid:   227298
  stop:  tide down token-net.tid
scenarios: 1 passed, 1 failed
token-net.tid:53:5: error: scenario 'bob_spends_alice' failed: transaction failed: Transaction simulation failed: Error processing Instruction 0: custom program error: 0x4
program logs:
  Program TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA invoke [1]
  Program log: Error: owner does not match
  Program TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA consumed 199 of 200000 compute units
  Program TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA failed: custom program error: 0x4
 53 |     transaction {
    |     ^
1 error(s), 0 warning(s)
3
```

How the failure is reported:

- An unbound transaction that fails aborts its block, so `print("not reached")`
  never ran. The diagnostic points at the `transaction` statement and
  includes the program logs, here SPL Token's `owner does not match`.
- A failing scenario does not stop the network or the other scenarios. The
  network is still running and the summary line counts passes and failures.
- `tide up` exits with code `3`: the network is up, but at least one scenario
  failed. Code `0` means everything passed, and code `1` means the network
  could not be built or started. See the [CLI reference](../reference/cli.md).

To make "bob cannot spend alice's tokens" a passing check instead, bind the
result and assert on it: `transaction as bad { ... }` followed by
`assert !bad.success;`.

You can run a subset of scenarios with `--scenario <name>` (repeatable) or
skip them with `--no-scenarios`:

```sh
tide up token-net.tid --scenario balances
```

```text
...
==> launching network
    starting solana-test-validator (network, rpc http://127.0.0.1:18899)
==> scenario 'balances'
    scenario 'balances' passed

network 'token_net' is running
  rpc:   http://127.0.0.1:18899
  state: .../.tide/token_net
  pid:   228568
  stop:  tide down token-net.tid
scenarios: 1 passed, 0 failed
```

The exit code is `0`.

## Step 6: stop the network

The network keeps running after `tide up` exits. Stop it with `tide down`,
passing the same file:

```sh
tide down token-net.tid
```

```text
stopping validator (pid 228568)
network 'token_net' stopped
```

`tide down` finds the network through the state directory (`.tide/token_net`
next to the script by default), so use the same `--state-dir` and
`--network` options you used with `tide up`. When nothing is running it
reports so and exits with code `1`:

```text
tide: no running network found in .../.tide/token_net
```

## The complete file

```tide
keypair alice = seed("alice");
keypair bob = seed("bob");
keypair usd = seed("usd");
let alice_usd = ata(alice.pubkey, usd.pubkey);
let bob_usd = ata(bob.pubkey, usd.pubkey);

network token_net {
  options {
    slots_per_epoch = 64;
    confirm_timeout = 30 s;
  }
  validator primary {
    rpc_port = 18899;
    faucet_port = 19900;
    gossip_port = 18001;
    dynamic_port_range = "18002-18040";
  }

  genesis {
    account alice.pubkey { lamports = 10 SOL; }
    account bob.pubkey { lamports = 2 SOL; }

    mint usd.pubkey { decimals = 6; mint_authority = alice.pubkey; supply = 1000000; }
    associated_token_account alice_usd { mint = usd.pubkey; owner = alice.pubkey; amount = 1000000; }
    associated_token_account bob_usd { mint = usd.pubkey; owner = bob.pubkey; amount = 0; }

    assert token_balance(alice_usd) == 1000000;
  }

  bootstrap {
    transaction as tx {
      signers = [alice];
      instruction token_program() {
        accounts = [alice_usd, bob_usd, alice.pubkey];
        data = spl_transfer(250000);
      }
      instruction memo_program() { accounts = []; data = memo("tide bootstrap"); }
    }
    assert tx.success, "transfer failed";
    print("bootstrap transfer:", tx.signature);
  }

  scenario balances {
    assert token_balance(bob_usd) == 250000;
    assert token_balance(alice_usd) == 750000;
  }

  // Deliberately failing: bob signs a transfer out of alice's account.
  scenario bob_spends_alice {
    transaction {
      signers = [bob];
      instruction token_program() {
        accounts = [alice_usd, bob_usd, bob.pubkey];
        data = spl_transfer(1);
      }
    }
    print("not reached");
  }
}
```

Next: the runtime statements used here are specified in
[runtime](../language/runtime.md), and the full lifecycle, including the
state directory, is in [lifecycle](../guides/lifecycle.md).
