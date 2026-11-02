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
check_exit "-o to an unwritable path -> 2"     2 "$verba" \
	-o /nonexistent-verba-dir-xyz/out.json echo hi
check_exit "-- separates options from command" 0 "$verba" -- echo hi

# --- fixture corpus ---
# The scratch dir lives under the project root (not /tmp), so undeclared writes
# into it are not swallowed by the built-in whitelist.
work="$PWD/.verba-test.$$"
mkdir -p "$work" "$work/zoo"
trap 'rm -rf "$work"' EXIT

chmod +x fixtures/*.sh 2>/dev/null

# honest control: faithful, exit 0, no findings.
out=$("$verba" --json ./fixtures/honest.sh "$work/honest.out" 2>&1)
code=$?
if [ "$code" -eq 0 ] && ! printf '%s' "$out" | grep -q '"type"'; then
	pass "honest fixture is faithful"
else
	bad "honest fixture (exit $code):"
	printf '%s\n' "$out"
fi

# liar-installer: must flag all three anomaly classes, exit 1.
out=$("$verba" --json ./fixtures/liar-installer.sh "$work/undeclared.out" 2>&1)
code=$?

if [ "$code" -eq 1 ]; then
	pass "liar fixture exits 1"
else
	bad "liar fixture exit (want 1, got $code)"
fi

check_type() {
	type="$1"
	if printf '%s' "$out" | grep -q "\"$type\""; then
		pass "liar fixture reports $type"
	else
		bad "liar fixture missing $type"
		printf '%s\n' "$out"
	fi
}

check_type faithless_claim
check_type undeclared_write
check_type discourse_mismatch

# crasher: success text then death by signal -> discourse mismatch, signaled.
out=$("$verba" --json ./fixtures/crasher.sh 2>&1)
code=$?
if [ "$code" -eq 1 ] && printf '%s' "$out" | grep -q 'discourse_mismatch'; then
	pass "crasher fixture: signal + success text -> discourse mismatch"
else
	bad "crasher fixture (exit $code):"
	printf '%s\n' "$out"
fi

# mistranslator: a phase-2 heuristic finding only (errno vs. claim).
# Default: heuristics off -> faithful, exit 0, no 'mistranslation' in output.
out=$("$verba" --json ./fixtures/mistranslator.sh 2>&1)
code=$?
if [ "$code" -eq 0 ] && ! printf '%s' "$out" | grep -q 'mistranslation'; then
	pass "mistranslator: silent without --heuristic/--strict"
else
	bad "mistranslator default (exit $code):"
	printf '%s\n' "$out"
fi

# --heuristic: report the mistranslation but keep the exit code mechanical (0).
out=$("$verba" --json --heuristic ./fixtures/mistranslator.sh 2>&1)
code=$?
if [ "$code" -eq 0 ] && printf '%s' "$out" | grep -q 'mistranslation' &&
	printf '%s' "$out" | grep -q '"heuristic": true'; then
	pass "mistranslator: --heuristic reports it, exit stays 0"
else
	bad "mistranslator --heuristic (exit $code):"
	printf '%s\n' "$out"
fi

# --strict: the heuristic finding now counts toward the exit code (1).
out=$("$verba" --json --strict ./fixtures/mistranslator.sh 2>&1)
code=$?
if [ "$code" -eq 1 ] && printf '%s' "$out" | grep -q 'mistranslation'; then
	pass "mistranslator: --strict fails (exit 1)"
else
	bad "mistranslator --strict (exit $code):"
	printf '%s\n' "$out"
fi

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
