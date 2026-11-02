# verba

> *Verba volant, syscalls manent.* — words fly away, syscalls remain.

`verba` runs a command and audits what it *says* against what it *does*: it
launches the command under `ptrace`, watches stdout/stderr and the exit code on
one side and the real file syscalls on the other, and reports every
contradiction.

## Status

Early development — **nothing is implemented yet.** This repository currently
holds the design and roadmap only. See [`TODO.md`](TODO.md) for the plan and the
anomaly classes verba aims to catch.

Decided: pure C, libc only, no runtime dependencies; traced via `ptrace` (no
`LD_PRELOAD`, so static and setuid binaries are in scope).
