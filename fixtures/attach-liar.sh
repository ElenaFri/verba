#!/bin/sh
# attach-liar — a target for `verba --follow`.
#
# verba does not launch this script; the harness (tests/run.sh) starts it, then
# attaches to it while it is running. It announces its PID, blocks on a FIFO
# until the auditor has attached, then changes into a scratch dir and makes a
# faithless claim — naming a *relative* path it never backs with a write.
#
# The relative path is the point: verba reads /proc/<pid>/cwd at the write(2),
# so it must resolve the claim against the dir this shell cd'd into, not against
# its own cwd. The echo is a shell builtin, so the writer is this very process.
#
# Usage: attach-liar.sh <pidfile> <fifo> <workdir>

pidfile="$1"
fifo="$2"
workdir="$3"

echo $$ > "$pidfile"
read _unblock < "$fifo"
cd "$workdir" || exit 3
# A line longer than verba's 4 KiB read chunk, to exercise the multi-chunk
# path of its write(2) capture. It names no path, so it yields no finding.
printf '%5000s\n' ''
echo "Saved configuration to ./attach-rel.conf"
