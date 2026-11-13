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
| **mistranslation** *(heuristic)* | error wording disagrees with the real `errno` — "permission denied" when the call actually failed with `ENOENT`, or "saved" when it failed |

The first three are **mechanical**: a disagreement between the output and the
kernel, reported by default. The last is a **heuristic**: it matches
error phrasings against a built-in `errno` dictionary (extend it with
`--patterns FILE`) and is only computed with `--heuristic` or `--strict` (see
below).

Writes under `/tmp`, `/dev`, `/proc`, `/sys`, `/run`, `$TMPDIR`, and to
non-files (pipes, sockets) are ignored as undeclared writes; add more with
`--allow PREFIX` (repeatable).

Some programs change what they say when stdout is not a terminal; `--tty`
captures them through a pty, at the price of one combined stream (`"tty"` in
JSON).

`--follow PID` audits a process that is already running: verba attaches to it
(`PTRACE_SEIZE`) and reconstructs its words from the `write(2)` calls it makes to
stdout and stderr. Only the target and the descendants it spawns after
attachment are followed, and it is left running when verba exits. Because the
writer's pid is known here, relative paths in a claim resolve against that
process's cwd at the moment it spoke (`/proc/PID/cwd`); in launch mode the merged
output stream cannot be attributed to one process, so relatives resolve against
verba's own cwd.

## Usage

```sh
verba [options] <command> [args...]

  -h, --help         show help and exit
  -V, --version      show version and exit
      --json         write the JSON report to stdout
  -o, --output FILE  write the JSON report to FILE (human summary still on stdout)
      --allow PREFIX  treat writes under PREFIX as declared (repeatable)
      --tty           run the command on a pty (stdout and stderr are then
                     captured as one combined stream) — for programs that
                     change what they say off-terminal
      --follow PID    attach to an already running process instead of launching
                     a command; its output is read from its write(2) calls
      --heuristic    also run the mistranslation checks (errno vs. claim),
                     reported in a separate block; does not affect the exit code
      --strict       run the heuristics and count them toward the exit code
      --patterns FILE  load extra errno↔phrase patterns from FILE (a JSON array
                       of {"phrase", "errno"} objects), used by the
                       mistranslation checks (repeatable)
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

The mechanical checks above run by default. The mistranslation heuristic is
opt-in: `--heuristic` reports it in its own block, `--strict` also makes it count
toward the exit code. Here a write fails because its parent directory is missing
(`ENOENT`), but the program blames a permission problem:

```console
$ verba --strict ./fixtures/mistranslator.sh
verba: no mechanical contradictions found.

verba: 1 heuristic finding (errno vs. claim):

  [mistranslation] /verba-no-such-dir-xyz/output.txt
      output reads like EACCES but the call failed with ENOENT
      words: "cannot create /verba-no-such-dir-xyz/output.txt: permission denied"
      deed:  openat failed with ENOENT

command exited with status 1.
```

Without a flag the same command stays silent: the wording is a judgement call,
not a mechanical fact, so it never pollutes the default report.

The errno dictionary is user-extensible: `--patterns FILE` (JSON, repeatable)
adds `"phrase"`/`"errno"` pairs — a French file mapping `"droits insuffisants"`
to `EACCES` lets the same check audit French-speaking programs
(`fixtures/mistranslator-fr.sh` demos it).

`--json` emits the same findings plus the full claim and trace tables under a
versioned schema (`"schema": 2`). Each finding carries a `"heuristic"` flag
(`true` for phase-2 mistranslations, `false` for the mechanical checks).

## Build

```sh
make           # build ./verba
make test      # unit tests + CLI smoke tests + the self-test fixture corpus
make install   # install to $PREFIX (default /usr/local)
make coverage  # local line-coverage report (lcov; coverage-html for HTML)
make bench     # time the tracing overhead against strace (needs strace)
```

`make install` also lays down the man page and the bash/zsh completion scripts
under `$PREFIX`. A minimal `debian/` dir wraps the same Makefile for Debian
packaging (`dpkg-buildpackage`).

## Status

See [`TODO.md`](TODO.md) for the roadmap.
