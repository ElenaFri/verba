#!/bin/sh
# verba test suite: CLI smoke tests plus the self-test fixture corpus.
#
# Usage: sh tests/run.sh [path-to-verba] [path-to-syscall-zoo]
# Run from the project root (the fixtures are referenced relative to cwd).

set -u

verba="${1:-./verba}"
zoo="${2:-}"
fail=0

pass() { printf 'ok:   %s\n' "$1"; }
bad() {
	printf 'FAIL: %s\n' "$1"
	fail=1
}

# --- CLI smoke tests ---

check_exit() {
	desc="$1"
	expected="$2"
	shift 2
	"$@" >/dev/null 2>&1
	got=$?
	if [ "$got" -eq "$expected" ]; then
		pass "$desc"
	else
		bad "$desc (want exit $expected, got $got)"
	fi
}

check_exit "--help exits 0"           0 "$verba" --help
check_exit "-h exits 0"               0 "$verba" -h
check_exit "--version exits 0"        0 "$verba" --version
check_exit "unknown option exits 2"   2 "$verba" --nope
check_exit "no command exits 2"       2 "$verba"
check_exit "unrunnable command -> 2"  2 "$verba" ./fixtures/does-not-exist-xyz

# Option parsing: missing arguments, the -- separator, and a bad output path.
check_exit "-o requires an argument -> 2"      2 "$verba" -o
check_exit "--allow requires an argument -> 2" 2 "$verba" --allow
check_exit "--patterns requires an argument -> 2" 2 "$verba" --patterns
check_exit "-o to an unwritable path -> 2"     2 "$verba" \
	-o /nonexistent-verba-dir-xyz/out.json echo hi
check_exit "-- separates options from command" 0 "$verba" -- echo hi

# --- fixture corpus ---
# The scratch dir lives under the project root (not /tmp), so undeclared writes
# into it are not swallowed by the built-in whitelist.
work="$PWD/.verba-test.$$"
mkdir -p "$work" "$work/zoo"
# Canonicalise: the tracer reports realpath()-resolved paths, so the expected
# findings must be compared against the physical spelling of the scratch dir.
work=$(cd "$work" && pwd -P)
trap 'rm -rf "$work"' EXIT

chmod +x fixtures/*.sh 2>/dev/null

# --- full expected-findings diffs ---
# Canonical form of a findings report: one "type path" line per finding
# (path "-" when null), the scratch dir normalised to WORK, sorted. A presence
# grep cannot catch a missing or an extra finding; this can.
findings_json() {
	awk -v work="$work" '
		/"type": "/ {
			t = $0; sub(/^.*"type": "/, "", t); sub(/".*$/, "", t);
			if (match($0, /"path": "/)) {
				p = $0; sub(/^.*"path": "/, "", p); sub(/".*$/, "", p);
				sub(work, "WORK", p)
			} else {
				p = "-"
			}
			print t " " p
		}' | sort
}

check_findings() {
	desc="$1"
	expected="$2"
	json="$3"
	got=$(printf '%s' "$json" | findings_json)
	if [ "$got" = "$expected" ]; then
		pass "$desc"
	else
		bad "$desc:"
		printf '%s\n' "--- expected ---" "$expected" "--- got ---" "$got"
	fi
}

# honest control: faithful, exit 0, no findings.
out=$("$verba" --json ./fixtures/honest.sh "$work/honest.out" 2>&1)
code=$?
if [ "$code" -eq 0 ] && ! printf '%s' "$out" | grep -q '"type"'; then
	pass "honest fixture is faithful"
else
	bad "honest fixture (exit $code):"
	printf '%s\n' "$out"
fi

# liar-installer: must flag exactly the three anomaly classes, exit 1.
out=$("$verba" --json ./fixtures/liar-installer.sh "$work/undeclared.out" 2>&1)
code=$?

if [ "$code" -eq 1 ]; then
	pass "liar fixture exits 1"
else
	bad "liar fixture exit (want 1, got $code)"
fi

check_findings "liar fixture reports exactly the three anomalies" 'discourse_mismatch -
faithless_claim /etc/verba-liar.conf
undeclared_write WORK/undeclared.out' "$out"

# static liar: the same three anomalies from a -static binary — the whole
# point of ptrace over LD_PRELOAD. Built by `make test`; skipped when the
# toolchain cannot link statically.
if [ -x tests/static-liar ]; then
	out=$("$verba" --json ./tests/static-liar "$work/static.out" 2>&1)
	code=$?
	if [ "$code" -eq 1 ]; then
		pass "static liar exits 1"
	else
		bad "static liar exit (want 1, got $code)"
	fi
	check_findings "static liar reports exactly the three anomalies" 'discourse_mismatch -
faithless_claim /etc/verba-static-liar.conf
undeclared_write WORK/static.out' "$out"
else
	printf 'skip: static fixture not built (no static libc?)\n'
fi

# make liar: each recipe runs in a shell make spawns — a realistic
# follow-fork/exec workload — lying the same three ways.
if command -v make >/dev/null 2>&1; then
	out=$("$verba" --json make -f fixtures/makefile-liar.mk all \
		OUT="$work/make.out" 2>&1)
	code=$?
	if [ "$code" -eq 1 ]; then
		pass "make liar exits 1"
	else
		bad "make liar exit (want 1, got $code)"
	fi
	check_findings "make liar reports exactly the three anomalies" 'discourse_mismatch -
faithless_claim /etc/verba-make-liar.conf
undeclared_write WORK/make.out' "$out"
else
	printf 'skip: make not available\n'
fi

# crasher: success text then death by signal -> discourse mismatch, signaled.
out=$("$verba" --json ./fixtures/crasher.sh 2>&1)
code=$?
if [ "$code" -eq 1 ]; then
	pass "crasher fixture exits 1"
else
	bad "crasher fixture exit (want 1, got $code)"
fi
check_findings "crasher: signal + success text -> discourse mismatch" 'discourse_mismatch -' "$out"

# mistranslator: a heuristic finding only (errno vs. claim).
# Default: heuristics off -> faithful, exit 0, no findings at all.
out=$("$verba" --json ./fixtures/mistranslator.sh 2>&1)
code=$?
if [ "$code" -eq 0 ]; then
	pass "mistranslator: silent without --heuristic/--strict"
else
	bad "mistranslator default (exit $code)"
fi
check_findings "mistranslator default reports nothing" '' "$out"

# --heuristic: report the mistranslation but keep the exit code mechanical (0).
out=$("$verba" --json --heuristic ./fixtures/mistranslator.sh 2>&1)
code=$?
if [ "$code" -eq 0 ]; then
	pass "mistranslator: --heuristic keeps the exit mechanical"
else
	bad "mistranslator --heuristic exit (want 0, got $code)"
fi
check_findings "mistranslator --heuristic reports exactly one mistranslation" 'mistranslation /verba-no-such-dir-xyz/output.txt' "$out"

# --strict: the heuristic finding now counts toward the exit code (1).
out=$("$verba" --json --strict ./fixtures/mistranslator.sh 2>&1)
code=$?
if [ "$code" -eq 1 ]; then
	pass "mistranslator: --strict fails (exit 1)"
else
	bad "mistranslator --strict exit (want 1, got $code)"
fi
check_findings "mistranslator --strict reports exactly one mistranslation" 'mistranslation /verba-no-such-dir-xyz/output.txt' "$out"

# A user-supplied errno<->phrase dictionary (--patterns, JSON) extends the
# mistranslation checks to non-English wording.
cat > "$work/fr-patterns.json" <<'EOF'
[
  {"phrase": "droits insuffisants", "errno": "EACCES"},
  {"phrase": "fichier introuvable", "errno": "ENOENT"}
]
EOF

# Without the dictionary the French wording is unreadable -> no finding.
out=$("$verba" --json --heuristic ./fixtures/mistranslator-fr.sh 2>&1)
code=$?
if [ "$code" -eq 0 ]; then
	pass "fr fixture: unreadable without --patterns"
else
	bad "fr fixture without --patterns (exit $code)"
fi
check_findings "fr fixture without --patterns reports nothing" '' "$out"

# With it, the same run flags the mistranslation (exit stays mechanical).
out=$("$verba" --json --heuristic --patterns "$work/fr-patterns.json" \
	./fixtures/mistranslator-fr.sh 2>&1)
code=$?
if [ "$code" -eq 0 ]; then
	pass "fr fixture: --patterns keeps the exit mechanical"
else
	bad "fr fixture with --patterns (want 0, got $code)"
fi
check_findings "fr fixture: --patterns reads the French wording" 'mistranslation /verba-no-such-dir-xyz/fr.txt' "$out"

# --strict + --patterns: the heuristic finding counts (exit 1).
out=$("$verba" --json --strict --patterns "$work/fr-patterns.json" \
	./fixtures/mistranslator-fr.sh 2>&1)
code=$?
if [ "$code" -eq 1 ]; then
	pass "fr fixture: --strict --patterns fails (exit 1)"
else
	bad "fr fixture --strict --patterns (want 1, got $code)"
fi
check_findings "fr fixture --strict reports exactly one mistranslation" 'mistranslation /verba-no-such-dir-xyz/fr.txt' "$out"

# A pattern file that cannot be read is a usage error.
check_exit "--patterns with an unreadable file -> 2" 2 "$verba" \
	--patterns ./fixtures/no-such-patterns-xyz.json echo hi

# Human report (no --json) with a finding: faithless claim, exit 1.
out=$("$verba" echo "Saved /no/such/verba/phantom.conf" 2>&1)
code=$?
if [ "$code" -eq 1 ] && printf '%s' "$out" | grep -q 'faithless claim'; then
	pass "human report shows a faithless claim"
else
	bad "human faithless-claim report (exit $code):"
	printf '%s\n' "$out"
fi

# -o writes a JSON report to a file and the human summary to stdout.
if "$verba" -o "$work/cli.json" echo hi >/dev/null 2>&1 &&
	grep -q '"schema": 2' "$work/cli.json" 2>/dev/null; then
	pass "-o writes a JSON report file"
else
	bad "-o did not write a JSON report file"
fi

# --allow is accepted and suppresses writes under its prefix.
check_exit "--allow PREFIX is accepted" 0 "$verba" --allow "$work" -- echo hi

# Output edge cases: a CRLF line ending (stripped) and a trailing partial line.
check_exit "CRLF and partial-line output handled" 0 \
	"$verba" /bin/sh -c "printf 'a\r\nb\nc'"

# syscall zoo: exercise the tracer's per-syscall handlers. It performs every
# traced file syscall (plain and *at forms) and leaves undeclared writes, so
# verba exits 1; we only require that it ran without a usage/internal error.
if [ -n "$zoo" ] && [ -x "$zoo" ]; then
	"$verba" --json "$zoo" "$work/zoo" >/dev/null 2>&1
	code=$?
	if [ "$code" -eq 0 ] || [ "$code" -eq 1 ]; then
		pass "syscall zoo traced cleanly"
	else
		bad "syscall zoo run failed (exit $code)"
	fi
fi

if [ "$fail" -ne 0 ]; then
	printf '\nsome tests failed\n'
	exit 1
fi

printf '\nall tests passed\n'
