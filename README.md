# verba

[![codecov](https://codecov.io/gh/ElenaFri/verba/branch/main/graph/badge.svg)](https://codecov.io/gh/ElenaFri/verba)

> *Verba volant, syscalls manent.* — words fly away, syscalls remain.

`verba` runs a command and audits what it *says* against what it *does*: it
launches the command under `ptrace`, watches stdout/stderr and the exit code on
one side and the real file syscalls on the other, and reports every
contradiction.

## Status

Early development. Phase 0 (scaffolding) is done: the project builds and ships a
CLI stub (`--help`, `--version`), a man page, and CI. The tracer and the audit
itself are **not implemented yet** — see [`TODO.md`](TODO.md) for the roadmap and
the anomaly classes verba aims to catch.

Decided: pure C, libc only, no runtime dependencies; traced via `ptrace` (no
`LD_PRELOAD`, so static and setuid binaries are in scope).

## Build

```sh
make           # build ./verba
make test      # run the smoke tests
make install   # install to $PREFIX (default /usr/local)
```

Builds cleanly with `-Wall -Wextra -Werror` under gcc and musl-gcc.
Run `make coverage` for a local line-coverage report (`lcov`/`genhtml`).
