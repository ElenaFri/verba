# verba — TODO / roadmap

> Mission: run a command, watch its words *and* its syscalls, report every
> contradiction between them. Words fly away; syscalls remain.

## Guiding constraints

- **ptrace, not LD_PRELOAD** : static and setuid binaries must be traced.
- **Zero runtime dependencies** : pure C, libc only.
- **No interpretation beyond the mechanical in the MVP** : phase 2+ only,
  behind an explicitly curated pattern database.
- **CI-friendly** : stable exit codes, JSON output, self-testable fixtures.

## Non-goals

- Malware / anomaly detection (Falco territory).
- Full syscall listing (strace territory) — we keep only what serves the audit.
- Network-claim auditing (v3 candidate, out of scope for now).

---

## Phase 0 — scaffolding

- [ ] Repo skeleton, Makefile (`all`, `test`, `clean`, `install`), MIT license.
- [ ] CI (GitHub Actions): build on gcc + musl-gcc, `-Wall -Wextra -Werror`, valgrind run.
- [ ] `verba --version`, `--help`, man page stub.

## Phase 1 — MVP: the mechanical audit

### 1.1 Tracer

- [ ] `fork` + `exec` under `PTRACE_TRACEME`, trace **enter + exit** of:
  `open`, `openat`, `creat`, `unlink`, `unlinkat`, `rename`, `renameat`,
  `link`, `symlink`, `mkdir`, `mkdirat` — plus `errno` on failure.
- [ ] Track `write` on fds already resolved to a file opened with
  `O_WRONLY|O_RDWR|O_CREAT` — a file opened but never written counts as
  *opened*, not *written*.
- [ ] Follow children: `PTRACE_O_TRACEFORK|TRACEVFORK|TRACECLONE|TRACEEXEC`.
  `make`-style scripts spawn helpers; they all speak for the program.
- [ ] Record per syscall: pid, path arguments, flags, result, errno. Resolve
  relative paths against the calling child's cwd — except `*at` variants, which
  resolve against their `dirfd` (`AT_FDCWD` = cwd).
- [ ] Read path strings out of the tracee's address space (`PTRACE_PEEKDATA` or
  `process_vm_readv`); handle paths that straddle word/page boundaries and
  guard against missing NUL terminators.
- [ ] Count `O_CREAT`/`O_TRUNC` (and `ftruncate`) as a *modification* even when
  no `write(2)` follows — truncating a file is changing it.
- [ ] Robustness: `PTRACE_O_EXITKILL` so tracees never outlive verba, forward
  signals to the child, and propagate its real exit status (see 1.5).
- [ ] Architecture portability: syscall numbers and the arg-register layout
  differ per arch. MVP targets x86-64; isolate the arch map so aarch64 can
  follow.

### 1.2 Output capture

- [ ] Capture stdout and stderr **unbuffered, interleaved**, tagged with a
  global order counter.
- [ ] Decision to make: pipes (simple, some programs change behavior when
  stdout is not a tty) vs pty (faithful, more complex). MVP: pipes; document
  the caveat. Revisit in phase 3.

### 1.3 Path extraction (the "words")

- [ ] Extract from program output: absolute paths, `./` and bare relative
  paths, `~/…`, `$HOME/…`.
- [ ] Resolve relative paths against the cwd **at the time of the claim**
  (commands print before or after acting — order matters).
- [ ] Canonicalize: symlink resolution, `..`, trailing slashes. Paths that
  are the same after canonicalization are the same word.

### 1.4 Reconciliation engine

Three checks, all mechanical, all reportable in MVP:

- [ ] **faithless claim** — output claims writing path P (heuristic: P appears
  in a sentence with a success/write verb — see pattern list below), but P
  was never opened for writing by any traced process.
- [ ] **undeclared write** — file created/modified, never mentioned in any
  output. Whitelist (config-overridable): `$TMPDIR`, `/tmp`, `/dev`,
  inherited fds, sockets, pipes, the tty.
- [ ] **discourse mismatch** — success-patterns in output vs exit code ≠ 0,
  and the reverse ("error"-patterns, exit 0).

Verb/pattern lists for phase 1 stay minimal and explicit in code:
`saved`, `written`, `created`, `wrote`.

- [ ] **Open decision:** do i18n verbs (FR `enregistré`, `écrit`, …) ship in
  phase 1, or wait for the phase 2 pattern DB? Leaning English-only in phase 1.

### 1.5 Report

- [ ] Human report: one block per finding — type, paths involved, evidence
  (the offending output line + the absent syscall, or vice versa).
- [ ] JSON output (`--json` / `-o <file>` — reconcile the spelling with the
  README): findings + full claim/trace tables, versioned schema.
- [ ] Exit codes: `0` faithful, `1` contradictions, `2` internal error.
- [ ] **Open decision:** how to surface the *child's* own exit status without
  colliding with verba's `0/1/2`? (e.g. `--exit-code=child`, or shift verba's
  codes). Matters for the CI-friendly constraint.

### 1.6 Self-test corpus

- [ ] `fixtures/` — scripts that deliberately lie each way: claim-without-write
  (classic installer), undeclared `~/.bashrc` touch, success+exit1, honest
  program (negative control), static binary fixture, `make`-spawning fixture.
- [ ] `make test` runs the corpus through verba and diffs against expected
  findings.

**Definition of done (MVP):** the corpus passes, valgrind clean,
`verba ./fixtures/liar-installer.sh` shows the three anomaly classes with
exact evidence lines, and the README's table is real.

## Phase 2 — mistranslation auditing (heuristic, opt-in flag)

- [ ] errno-vs-claim reconciliation: program prints "file not found" while the
  failed call was `EACCES`; "saved" while the call returned `EPERM`.
- [ ] Community pattern database (TOML/JSON, user-extensible — the
  *faux-amis dictionary* of error phrasings, English first).
- [ ] Reports tagged `heuristic` — never mixed with mechanical findings
  in default output; `--strict` fails on them too.

## Phase 3 — polish

- [ ] pty capture mode (`--tty`) for programs that change behavior off-terminal.
- [ ] `--follow PID` (attach) — stretch goal, changes the threat model of the audit.
- [ ] Shell-completion, man page complete, packaging (deb, AUR).
- [ ] Performance: trace enter-only where exit data is not needed;
  benchmark against `strace -f -e trace=%file`.

---

*Status: phase 0 not started. This file will be updated before each release.*
