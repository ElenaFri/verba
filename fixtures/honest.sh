#!/bin/sh
# Honest control fixture.
#
# Everything it says, it does; everything it does, it says. verba must find
# zero contradictions and exit 0.
#
# $1 = scratch output path (lives under the project root, not /tmp, so writes
#      there are not swallowed by the built-in whitelist).

set -eu

out="$1"

# Do the deed, then declare exactly that deed.
printf 'hello from verba\n' > "$out"
echo "Saved results to $out"
echo "Done."

exit 0
