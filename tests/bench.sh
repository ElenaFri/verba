#!/bin/sh
# verba benchmark: measure tracing overhead against strace.
#
# Usage: sh tests/bench.sh [path-to-verba] [units]
# Run from the project root. Builds nothing; expects ./verba and
# tests/bench_workload to exist (see `make bench`).
#
# It times a deterministic, syscall-heavy workload three ways: bare, under
# verba, and under `strace -f -e trace=%file` (the closest strace equivalent of
# what verba watches). strace is skipped with a note if it is not installed.

set -u

verba="${1:-./verba}"
units="${2:-200000}"
work="tests/bench_workload"

if [ ! -x "$verba" ] || [ ! -x "$work" ]; then
	echo "bench: need $verba and $work (run 'make bench')" >&2
	exit 2
fi

# Scratch under $PWD (never /tmp), cleaned on exit.
scratch=".verba-bench.$$"
mkdir -p "$scratch" || exit 2
trap 'rm -rf "$scratch"' EXIT

# Portable wall-clock median of 3 runs, in integer milliseconds.
now_ms() {
	# date +%s%N is nanoseconds since epoch; /1e6 -> ms.
	date +%s%N 2>/dev/null | awk '{printf "%d", $1/1000000}'
}

time_cmd() {
	best=""
	i=0
	while [ "$i" -lt 3 ]; do
		t0=$(now_ms)
		"$@" >/dev/null 2>&1
		t1=$(now_ms)
		d=$((t1 - t0))
		if [ -z "$best" ] || [ "$d" -lt "$best" ]; then
			best=$d
		fi
		i=$((i + 1))
	done
	echo "$best"
}

echo "workload: $units units (burst of 10 getpid each, one file op per 100)"
echo

bare=$(time_cmd "$work" "$scratch" "$units")
echo "bare:   ${bare} ms"

vb=$(time_cmd "$verba" -o /dev/null "$work" "$scratch" "$units")
printf 'verba:  %s ms' "$vb"
if [ "$bare" -gt 0 ]; then
	printf '  (%sx bare)\n' "$(awk "BEGIN{printf \"%.1f\", $vb/$bare}")"
else
	echo
fi

if command -v strace >/dev/null 2>&1; then
	st=$(time_cmd strace -f -e trace=%file -o /dev/null \
		"$work" "$scratch" "$units")
	printf 'strace: %s ms' "$st"
	if [ "$bare" -gt 0 ]; then
		printf '  (%sx bare)\n' "$(awk "BEGIN{printf \"%.1f\", $st/$bare}")"
	else
		echo
	fi
else
	echo "strace: not installed — skipped"
fi

echo
echo "note: verba and strace -f -e trace=%file both stop on every syscall the"
echo "tracee makes, so their overhead scales with total syscall count, not just"
echo "the file ops. A seccomp-BPF prefilter (future work) would cut the stops"
echo "down to the ~15 watched syscalls."
