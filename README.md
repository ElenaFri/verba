# verba

> *Verba volant, syscalls manent.* — words fly away, syscalls remain.

`verba` runs a command and audits what it *says* against what it *does*.

A CLI program is a translator between you and the kernel: it makes claims
("saved successfully"), the kernel executes. verba is the auditor of that
translation. It launches a command under `ptrace`, watches both sides of the
conversation (stdout/stderr and exit code on one side, real syscalls on the
other) and reports every contradiction.

## What it catches

| The program… | verba reports |
| --- | --- |
| claims to write `/etc/app.conf`, never opens it | `faithless claim` |
| writes to `~/.bashrc`, says nothing about it | `undeclared write` |
| prints "success", exits 1 (or the reverse) | `discourse mismatch` |
| prints "file not found", but the call failed with `EACCES` | `mistranslation` *(roadmap)* |

## Usage

```
verba make install
verba -o report.json ./setup.sh
```

CI-friendly exit codes: `0` = faithful, `1` = contradictions found, `2` = internal error.

## How it works

- Traces file-related syscalls (`open`, `openat`, `write`, `unlink`, `rename`,
  failed calls) via `ptrace` — no `LD_PRELOAD`, so static and setuid binaries
  are traced too.
- Extracts file paths from the program's output and reconciles them against
  the trace.
- Human-readable report by default, `-o json` for machines.

## Status

Early development. Written in C, no runtime dependencies.
