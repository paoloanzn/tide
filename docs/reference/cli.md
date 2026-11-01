# Command-line reference

The `tide` executable has three commands: `check` validates a script, `up`
runs it and leaves a network running, and `down` stops that network. This page
lists every command and option, the exit codes, and the format of diagnostics.

## Synopsis

```sh
tide check <file.tid> [options]
tide up    <file.tid> [options]
tide down  <file.tid> [options]
tide --help
tide --version
```

Options may appear before, between or after the command and the file. Exactly
two positional arguments are required: the command and the file.

## Commands

### `tide check`

Parses the file and its imports and runs the semantic checker. Nothing is
executed: no bindings are evaluated, no genesis is built, no files are written
and no process is started. All networks in the file are checked.

```sh
tide check net.tid
```

```text
net.tid: ok (1 network(s))
```

The `ok` line goes to stdout and names the file as it was given on the command
line. Warnings, if any, follow on stderr with a [summary line](#summary-line).

`check` does not catch problems that only appear when the script runs, such as
a failed `assert`, a missing program binary, or a construct the
[backend](../backends/solana-test-validator.md#limitations) cannot express.
`tide up --dry-run` catches those that occur during configuration and genesis.

### `tide up`

Runs the full lifecycle on one network: configuration, genesis, optional
bootstrap, persist, launch, scenarios, then exits while the network keeps
running. Each phase is described in [the lifecycle guide](../guides/lifecycle.md).

```sh
tide up net.tid
tide up net.tid --network staging --scenario smoke
tide up net.tid --dry-run
```

On success it prints the RPC URL, state directory, pid and the matching
`tide down` command.

### `tide down`

Stops a network previously started by `tide up`, using the pid recorded in
`<state dir>/<network>/validator.pid`. The file is still parsed and checked,
because the network name and default state directory come from it, so it must
be a valid script.

```sh
tide down net.tid
```

```text
stopping validator (pid 218042)
network 'e2e' stopped
```

Pass the same `--network` and `--state-dir` as for `tide up`. See
[`tide down`](../guides/lifecycle.md#tide-down) for the exact stop sequence.

### `--help` and `--version`

| Invocation | Output | Exit code |
|------------|--------|-----------|
| `tide --help`, `tide -h`, `tide help` | usage text on stdout | 0 |
| `tide --version`, `tide version` | `tide 0.1.0` on stdout | 0 |

These are recognised only as the first argument. `tide check net.tid --help`
is an unknown option (exit code 2).

## Options

| Option | Commands | Default | Description |
|--------|----------|---------|-------------|
| `--network <name>` | `up`, `down` | the only network | Network to use when the file declares several. See [network selection](#network-selection). |
| `--scenario <name>` | `up` | all scenarios | Run only this scenario. Repeatable. Names that match no scenario are ignored. |
| `--no-scenarios` | `up` | off | Skip all scenarios. Takes precedence over `--scenario`. |
| `--dry-run` | `up` | off | Build and persist genesis state, print the validator command line, start nothing. See [`--dry-run`](../guides/lifecycle.md#--dry-run). |
| `--state-dir <dir>` | `up`, `down` | `<script dir>/.tide` | Root directory for network state. Each network uses `<dir>/<network>/`. A relative path is relative to the current directory. |
| `--validator <path>` | `up` | `solana-test-validator` from `PATH` | Validator binary. A value containing `/` is used as a path. |
| `--solana <path>` | `up` | `solana` from `PATH` | Solana CLI binary, used only by the `deploy` statement. |

Every option is accepted by every command; options that do not apply to a
command are ignored (for example `--dry-run` with `check`). An option that
takes a value and is the last argument is an error:

```text
tide: --network requires a value
```

## Network selection

`up` and `down` act on one network. `check` validates all of them and needs no
selection.

1. With `--network <name>`, that network is used. If the file declares no
   network with that name: `tide: no network named '<name>' in <file>`.
2. Without `--network`, if the file declares exactly one network, it is used.
3. Otherwise the command fails, listing the declared networks:

```text
tide: the file declares several networks; pick one with --network: alpha beta
```

```text
tide: the file declares no network
```

All selection errors exit with code 2. Only networks declared in the file
given on the command line can be selected, not networks in imported modules.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success. `check` found no errors; `up` reached the end of the lifecycle (or the dry run persisted) and every scenario that ran passed; `down` stopped the network or found its process already gone; `--help`/`--version`. Warnings do not change the code. |
| 1 | An error in the script or during execution: the file cannot be read, parse or check errors, an evaluation error, a backend error (such as a busy port or a validator that fails to start), a failed `assert` in genesis or bootstrap. For `down`: no `validator.pid` was found, or it is corrupt. |
| 2 | Usage error: missing command or file, unknown command, unknown option, option missing its value, or a network selection error. Usage errors that come from argument parsing also print the usage text. |
| 3 | `up` only: the network was launched, but at least one scenario failed. The network is still running. |

## Output streams

- Progress (`==> configuring network ...`), `print(...)` output from scripts,
  the `ok` line of `check` and the final connection details go to **stdout**.
- Diagnostics, the summary line and usage errors go to **stderr**.
- Diagnostics are collected while the command runs and printed together when
  it finishes, so in a terminal they appear after the progress output.

## Diagnostics

A diagnostic has a severity (`error`, `warning` or `note`), a source location,
a message, and optionally one or more notes pointing at related locations.

### Format

```text
<file>:<line>:<col>: <severity>: <message>
 <line> | <source line>
        | <caret and underline>
```

- `<file>` is the absolute path of the file containing the location (the
  examples on this page shorten it). Line and column are 1-based.
- The second line repeats the source line, prefixed by its line number.
- The third line places `^` under the first character of the location and
  `~` under the rest of it. A location that covers a whole token or expression
  (such as an identifier) is underlined in full; a point location (such as the
  start of a statement) shows only `^`. The underline never extends past the
  end of the line.
- Each note follows its diagnostic in the same format, with severity `note`.
- A diagnostic without a source location is printed as `tide: <severity>: <message>`
  with no source lines.

### Examples

An unknown name close to a known one gets a "did you mean" suggestion. The
suggestion is offered when the edit distance to a known name is small
(at most `max(2, length / 3)`):

```tide
keypair alice = seed("alice");

network dev {
  genesis {
    account alice.pubkey { lamports = 10 SOL; }
    assert balanse(alice.pubkey) == 10 SOL;
  }
}
```

```text
net.tid:6:12: error: unknown function 'balanse'; did you mean 'balance'?
 6 |     assert balanse(alice.pubkey) == 10 SOL;
   |            ^~~~~~~
1 error(s), 0 warning(s)
```

A parse error points at the position where the parser expected something
else:

```tide
network dev {
  genesis {
    account seed("a").pubkey { lamports = 1 SOL }
  }
}
```

```text
net.tid:3:48: error: expected ';' in record entry
 3 |     account seed("a").pubkey { lamports = 1 SOL }
   |                                                ^
1 error(s), 0 warning(s)
```

A diagnostic with a note. This one comes from `tide up` (the duplicate is
detected while genesis runs), so the progress lines appear first on stdout:

```tide
keypair alice = seed("alice");

network dev {
  genesis {
    account alice.pubkey { lamports = 1 SOL; }
    account alice.pubkey { lamports = 2 SOL; }
  }
}
```

```text
==> configuring network 'dev'
==> building genesis state
net.tid:6:13: error: address HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye is already defined in genesis (as account)
 6 |     account alice.pubkey { lamports = 2 SOL; }
   |             ^~~~~
net.tid:5:13: note: previous definition is here
 5 |     account alice.pubkey { lamports = 1 SOL; }
   |             ^~~~~
1 error(s), 0 warning(s)
```

Warnings do not stop the command or change the exit code (this run exits
with 0):

```text
fork.tid:5:5: warning: 'at slot' is ignored: the solana-test-validator backend copies the current feature set of the cluster
 5 |     fork from upstream at slot 300000000;
   |     ^
fork.tid:5:5: warning: fork copies the cluster's feature set only; clone the accounts you need explicitly
 5 |     fork from upstream at slot 300000000;
   |     ^
0 error(s), 2 warning(s)
```

### Summary line

When at least one error or warning was reported, the last line on stderr is:

```text
<N> error(s), <M> warning(s)
```

It is omitted when there are neither. Notes are not counted. A failed
scenario counts as an error even though `tide up` exits with code 3.

Messages printed directly by the driver, such as usage errors,
`tide: cannot read '<file>'` and the network selection errors above, are not
diagnostics: they have no location and are not counted in the summary.

### Color

Diagnostics are colored when stderr is a terminal (`isatty`), and plain
otherwise, for example when piped or redirected to a file. There is no option
or environment variable to change this.

| Part | Style |
|------|-------|
| location and message | bold |
| `error` and its underline | bold red |
| `warning` and its underline | bold magenta |
| `note` and its underline | bold cyan |

See also: [the lifecycle guide](../guides/lifecycle.md),
[solana-test-validator backend](../backends/solana-test-validator.md).
