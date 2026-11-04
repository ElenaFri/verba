#!/bin/sh
# tty fixture: part of what it says depends on stdout being a terminal.
#
# Over pipes (verba's default) the gated line never appears, so only --tty
# reveals the faithless claim. The rest lies the usual ways: an undeclared
# write and success text before a non-zero exit.
#
# $1 = scratch output path (under the project root, not whitelisted).

set -u

out="$1"

if [ -t 1 ]; then
	echo "Saved configuration to /etc/verba-tty-liar.conf"
fi

printf payload > "$out"
echo "Done."

exit 1
