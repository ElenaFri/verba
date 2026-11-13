#!/bin/sh
# attach-liar — a target for `verba --follow`.
#
# verba does not launch this script; the harness (tests/run.sh) starts it, then
# attaches to it while it is running. It announces its PID, blocks on a FIFO
# until the auditor has attached, then makes a faithless claim it never backs
# with a write. The claim is reconstructed by verba from the write(2) to stdout.
#
# Usage: attach-liar.sh <pidfile> <fifo> <claimed-path>

pidfile="$1"
fifo="$2"
target="$3"

echo $$ > "$pidfile"
read _unblock < "$fifo"
# A line longer than verba's 4 KiB read chunk, to exercise the multi-chunk
# path of its write(2) capture. It names no path, so it yields no finding.
printf '%5000s\n' ''
echo "Saved configuration to $target"
