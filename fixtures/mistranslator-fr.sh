#!/bin/sh
# Heuristic fixture for --patterns: the error wording is French, so it only
# becomes readable once a user-supplied errno<->phrase dictionary is loaded.
#
# The write below fails with ENOENT (the parent directory does not exist), but
# the program blames a permission problem ("droits insuffisants" = EACCES in
# the test dictionary). Without --patterns the phrasing is unknown: no finding.

set -u

bad="/verba-no-such-dir-xyz/fr.txt"

{ printf 'data' > "$bad"; } 2>/dev/null || true
echo "echec d'ecriture dans $bad : droits insuffisants"

exit 1
