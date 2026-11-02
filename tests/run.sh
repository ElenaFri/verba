#!/bin/sh
# verba test suite: CLI smoke tests plus the self-test fixture corpus.
#
# Usage: sh tests/run.sh [path-to-verba]
# Run from the project root (the fixtures are referenced relative to cwd).

set -u

verba="${1:-./verba}"
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

# --- fixture corpus ---
# The scratch dir lives under the project root (not /tmp), so undeclared writes
# into it are not swallowed by the built-in whitelist.
work="$PWD/.verba-test.$$"
mkdir -p "$work"
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

if [ "$fail" -ne 0 ]; then
	printf '\nsome tests failed\n'
	exit 1
fi

printf '\nall tests passed\n'
