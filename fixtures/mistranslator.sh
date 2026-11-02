#!/bin/sh
# Heuristic (Phase 2) fixture: the error wording contradicts the real errno.
#
# The write below fails with ENOENT (the parent directory does not exist), but
# the program blames a permission problem (EACCES). That is a mistranslation:
# a *heuristic* finding, never a mechanical one. "cannot create" is not a write
# verb, a failed open is not a write, and error-text + exit 1 is consistent, so
# the phase-1 checks stay silent and only the phase-2 heuristic fires.

set -u

bad="/verba-no-such-dir-xyz/output.txt"

{ printf 'data' > "$bad"; } 2>/dev/null || true
echo "cannot create $bad: permission denied"

exit 1
