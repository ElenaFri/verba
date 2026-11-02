#!/bin/sh
# Liar fixture: a bogus "installer" that exhibits all three Phase-1 anomalies.
#
# $1 = scratch output path (under the project root, not whitelisted) that we
#      write WITHOUT ever naming it in the output.

set -u

scratch="$1"

# (1) faithless claim: we SAY we saved a config file we never touch.
echo "Saved configuration to /etc/verba-liar.conf"

# (2) undeclared write: a *child* process writes a real file we never mention.
#     The child also exercises verba's follow-fork/exec handling.
sh -c 'printf payload > "$1"' _ "$scratch"

# (3) discourse mismatch: announce success, then exit non-zero anyway.
echo "Installation completed successfully."

exit 1
