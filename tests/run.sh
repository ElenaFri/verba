#!/bin/sh
# Phase 0 smoke tests for the verba CLI surface.
#
# Usage: sh tests/run.sh [path-to-verba]
# Phase 1.6 will extend this harness to drive the fixtures/ corpus.

set -u

verba="${1:-./verba}"
fail=0

# check_exit DESC EXPECTED CMD...
check_exit() {
	desc="$1"
	expected="$2"
	shift 2
	"$@" >/dev/null 2>&1
	got=$?
	if [ "$got" -ne "$expected" ]; then
		printf 'FAIL: %s (expected exit %s, got %s)\n' "$desc" "$expected" "$got"
		fail=1
	else
		printf 'ok:   %s\n' "$desc"
	fi
}

# check_contains DESC NEEDLE CMD...
check_contains() {
	desc="$1"
	needle="$2"
	shift 2
	out=$("$@" 2>&1)
	case "$out" in
	*"$needle"*)
		printf 'ok:   %s\n' "$desc"
		;;
	*)
		printf 'FAIL: %s (output missing "%s")\n' "$desc" "$needle"
		fail=1
		;;
	esac
}

check_exit "--help exits 0"            0 "$verba" --help
check_exit "-h exits 0"                0 "$verba" -h
check_exit "--version exits 0"         0 "$verba" --version
check_exit "-V exits 0"                0 "$verba" -V
check_exit "unknown option exits 2"    2 "$verba" --nope
check_exit "no command exits 2"        2 "$verba"
check_exit "command exits 2 (stub)"    2 "$verba" true
check_exit "-- then command exits 2"   2 "$verba" -- true

check_contains "--version prints name"  "verba" "$verba" --version
check_contains "--help prints usage"    "Usage" "$verba" --help

if [ "$fail" -ne 0 ]; then
	printf '\nsome tests failed\n'
	exit 1
fi

printf '\nall tests passed\n'
