# verba

[![codecov](https://codecov.io/gh/ElenaFri/verba/branch/main/graph/badge.svg)](https://codecov.io/gh/ElenaFri/verba)

> *Verba volant, syscalls manent.* — words fly away, syscalls remain.

`verba` runs a command under `ptrace` and audits what it *says* against what it
*does*: it watches stdout/stderr and the exit code on one side and the real file
syscalls on the other, then reports every contradiction between the two.

Pure C, libc only, no runtime dependencies. Traced via `ptrace` (not
`LD_PRELOAD`), so static and setuid binaries are in scope. x86-64 only for now.

## What it catches

| finding | meaning |
| --- | --- |
| **faithless claim** | output claims a write to a path that no traced process ever opened for writing |
| **undeclared write** | a file was created/modified/removed on disk, but no output line mentions it |
| **discourse mismatch** | output says "success" while the command failed (exit ≠ 0), or says "error" while it exited 0 |

Writes under `/tmp`, `/dev`, `/proc`, `/sys`, `/run`, `$TMPDIR`, and to
non-files (pipes, sockets) are ignored as undeclared writes; add more with
`--allow PREFIX` (repeatable).

## Usage

```sh
verba [options] <command> [args...]

  -h, --help         show help and exit
  -V, --version      show version and exit
      --json         write the JSON report to stdout
  -o, --output FILE  write the JSON report to FILE (human summary still on stdout)
      --allow PREFIX  treat writes under PREFIX as declared (repeatable)
```

Exit codes: `0` faithful · `1` contradictions found · `2` usage/internal error.
The traced command's own exit status is reported (`exit.code` in JSON), not
propagated as verba's exit code.

### Example

```console
$ verba ./fixtures/liar-installer.sh ./out.txt
verba: 3 contradictions between words and syscalls:

  [faithless claim] /etc/verba-liar.conf
      output claims a write that never reached the kernel
      words: "Saved configuration to /etc/verba-liar.conf"
      deed:  no traced process opened /etc/verba-liar.conf for writing

  [undeclared write] /.../out.txt
      a file was created or modified but no output mentions it
      deed:  openat /.../out.txt

  [discourse mismatch]
      output reports success but the command failed
      words: "Installation completed successfully."
      deed:  exited with status 1

command exited with status 1.
```

`--json` emits the same findings plus the full claim and trace tables under a
versioned schema (`"schema": 1`).

## Build

```sh
make           # build ./verba
make test      # CLI smoke tests + the self-test fixture corpus
make install   # install to $PREFIX (default /usr/local)
make coverage  # local line-coverage report (lcov; coverage-html for HTML)
```

Builds cleanly with `-Wall -Wextra -Werror` under gcc and musl-gcc; the test
corpus runs valgrind-clean under CI.

## Status

See [`TODO.md`](TODO.md) for the roadmap.
