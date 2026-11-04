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

- [x] Repo skeleton, Makefile (`all`, `test`, `clean`, `install`), MIT license.
- [x] CI (GitHub Actions): build on gcc + musl-gcc, `-Wall -Wextra -Werror`, valgrind run.
- [x] `verba --version`, `--help`, man page stub.

## Phase 1 — MVP: the mechanical audit

### 1.1 Tracer

- [x] `fork` + `exec` under `PTRACE_TRACEME`, trace **enter + exit** of:
  `open`, `openat`, `creat`, `unlink`, `unlinkat`, `rename`, `renameat`
  (+ `renameat2`), `link`, `linkat`, `symlink`, `symlinkat`, `mkdir`,
  `mkdirat` — plus `errno` on failure. Enter/exit are disambiguated with
  `PTRACE_GET_SYSCALL_INFO`, with the entry/exit toggle kept as a kernel < 5.3
  fallback.
- [x] Track `write` on fds already resolved to a file opened with
  `O_WRONLY|O_RDWR|O_CREAT` — a file opened but never written counts as
  *opened*, not *written*.
- [x] Follow children: `PTRACE_O_TRACEFORK|TRACEVFORK|TRACECLONE|TRACEEXEC`.
  `make`-style scripts spawn helpers; they all speak for the program.
- [x] Record per syscall: pid, path arguments, flags, result, errno. Resolve
  relative paths against the calling child's cwd — except `*at` variants, which
  resolve against their `dirfd` (`AT_FDCWD` = cwd). Done via `/proc/<pid>/cwd`
  and `/proc/<pid>/fd/<dirfd>`.
- [x] Read path strings out of the tracee's address space (`PTRACE_PEEKDATA`,
  word-by-word); handles paths that straddle word boundaries and missing NUL
  terminators.
- [x] Count `O_CREAT`/`O_TRUNC` (and `ftruncate`) as a *modification* even when
  no `write(2)` follows — truncating a file is changing it. (`O_CREAT` counts
  only when the target did not already exist, checked with `lstat` at enter.)
- [x] Robustness: `PTRACE_O_EXITKILL` so tracees never outlive verba, forward
  signals to the child, and record its real exit status (surfaced in the
  report; see the 1.5 note on how it relates to verba's own exit code).
- [x] Architecture portability: syscall numbers and the arg-register layout
  differ per arch. MVP targets x86-64 (`arch.h`, `#error` elsewhere); the arch
  map is isolated so aarch64 can follow.

### 1.2 Output capture

- [x] Capture stdout and stderr **interleaved**, tagged with a global order
  counter, split into lines (`signalfd` + `poll` drain loop).
- [x] Decision made: **pipes** in the MVP (simple; some programs change behavior
  when stdout is not a tty — documented caveat). pty capture is deferred to
  phase 3.

### 1.3 Path extraction (the "words")

- [x] Extract from program output: absolute paths, `./` and bare relative
  paths (those containing a slash), `~/…`, `$HOME/…`; URLs are skipped.
- [x] Canonicalize: `..`, trailing slashes, symlink resolution. Paths that are
  the same after canonicalization are the same word.
- [~] Resolve relative paths against the cwd **at the time of the claim**.
  MVP resolves against a single cwd (verba's startup cwd, shared with the
  child); per-claim cwd tracking across a child `chdir` is deferred.

### 1.4 Reconciliation engine

Three checks, all mechanical, all reportable in MVP:

- [x] **faithless claim** — output claims writing path P (P appears in a line
  with a write/success verb), but P was never opened for writing by any traced
  process.
- [x] **undeclared write** — file created/modified, never mentioned in any
  output. Whitelist: `/tmp`, `/dev`, `/proc`, `/sys`, `/run`, `$TMPDIR`,
  non-absolute targets (pipes/sockets/anon fds), plus user `--allow PREFIX`.
- [x] **discourse mismatch** — success-patterns in output vs exit code ≠ 0,
  and the reverse ("error"-patterns, exit 0).

Verb/pattern lists for phase 1 stay minimal and explicit in code
(`src/words.c`): write verbs (`saved`, `wrote`, `created`, …), success words,
error words.

- [x] **Decision made:** English-only in phase 1. i18n verbs (FR `enregistré`,
  `écrit`, …) wait for the phase 2 pattern DB.

### 1.5 Report

- [x] Human report: one block per finding — type, paths involved, evidence
  (the offending output line + the absent/extra syscall).
- [x] JSON output (`--json` to stdout, `-o FILE` to a file): findings plus the
  claim and trace tables, versioned schema (`schema: 1`).
- [x] Exit codes: `0` faithful, `1` contradictions, `2` usage/internal error.
- [x] **Decision made (for now):** verba's exit code stays findings-based
  (`0/1/2`); the child's own exit status is reported in the JSON
  (`exit.code` / `exit.signal`) but not propagated as verba's code. A
  `--exit-code=child` style switch is deferred to a later phase.

### 1.6 Self-test corpus

- [x] `fixtures/honest.sh` — honest negative control (says what it does, exit 0).
- [x] `fixtures/liar-installer.sh` — a bogus installer that trips all three
  anomaly classes at once: claim-without-write, a child-spawned undeclared
  write (also exercises follow-fork), and success-text + exit 1.
- [x] `fixtures/crasher.sh` — success text, then death by signal: exercises the
  signal-delivery/signaled-termination path and the "killed by signal" deed.
- [x] `make test` runs the CLI smoke tests and the corpus through verba,
  asserting the expected finding types and exit codes (`tests/run.sh`), plus
  option-parsing edge cases (`-o`, `--allow`, `--`, missing arguments).
- [x] `tests/unit.c` — a C unit harness over the pure-logic modules (util,
  words, model, reconcile, report, vec), linked against the same instrumented
  objects as `./verba` so it counts toward coverage.
- [x] `tests/syscall_zoo.c` — a coverage helper, run under verba, that performs
  every traced file syscall in both the plain and the `*at` spelling (raw
  `syscall(2)`, since the libc wrappers often emit only the `*at` form), driving
  the tracer's enter/exit handlers.
- [x] Line coverage ≥ 95% (lcov ≈ 99.9%), measured by `make coverage` and
  uploaded to Codecov; genuinely defensive or unreachable branches are
  annotated with `LCOV_EXCL` and explained in a comment.
- [x] Still wanted: a dedicated static-binary fixture and a `make`-spawning
  fixture; full expected-findings diffs rather than type-presence assertions.
  Done: `tests/static-liar.c` (built `-static` by `make test`, tolerated and
  skipped when the toolchain cannot link statically), `fixtures/makefile-liar.mk`
  (skipped when `make` is absent), and `tests/run.sh` now diffs the canonical
  type+path set of every fixture's findings against the expected corpus
  (`findings_json`/`check_findings`) instead of grepping for type presence.

**Definition of done (MVP):** met — the corpus passes, the build is valgrind
clean, `verba ./fixtures/liar-installer.sh <path>` shows the three anomaly
classes with their evidence lines, and the README's table is real.

## Phase 2 — mistranslation auditing (heuristic, opt-in flag)

- [x] errno-vs-claim reconciliation: program prints "file not found" while the
  failed call was `EACCES`; "saved" while the call returned `EPERM`.
  (`src/heuristic.c`; the failed write-open events the tracer already records
  are matched, per named path, against a built-in errno↔phrase dictionary.)
- [x] Community pattern database (user-extensible — the *faux-amis dictionary*
  of error phrasings). **Decision made:** JSON, parsed by a small built-in
  parser — the zero-dependency rule rules out TOML libraries; loaded with
  `--patterns FILE` (repeatable) on top of the curated built-in table. This
  also unlocks non-English wording (see `fixtures/mistranslator-fr.sh`);
  a shared, community-maintained dictionary file is a later distribution
  question, the loading mechanism is done.
- [x] Reports tagged `heuristic` — never mixed with mechanical findings
  in default output; `--heuristic` reports them in a separate block without
  changing the exit code, `--strict` also fails on them.

## Phase 3 — polish

- [x] pty capture mode (`--tty`) for programs that change behavior
  off-terminal. The command runs as a session leader on a pty slave
  (`posix_openpt`/`grantpt`/`unlockpt`), stdout and stderr arrive as one
  combined stream tagged `tty` in JSON; EOF on the master (EIO) replaces the
  pipes' EOF. The deferred caveat from phase 1.2 is closed.
- [ ] `--follow PID` (attach) — stretch goal, changes the threat model of the audit.
- [ ] Shell-completion, man page complete, packaging (deb, AUR).
- [ ] Performance: trace enter-only where exit data is not needed;
  benchmark against `strace -f -e trace=%file`.

---

*Status: phase 0 complete; phase 1 (the mechanical audit) complete and
valgrind-clean — verba traces a command and reports the three anomaly classes,
with the test suite at ≈ 99.9% line coverage. Phase 2 (opt-in, heuristic
mistranslation auditing: errno vs. claim, behind `--heuristic`/`--strict`)
complete — built-in errno↔phrase dictionary plus a user-extensible JSON one
(`--patterns FILE`). Phase 3 under way: pty capture (`--tty`) is done; next:
performance (enter-only tracing, benchmark vs strace), packaging, `--follow`.
This file is updated before each release.*
