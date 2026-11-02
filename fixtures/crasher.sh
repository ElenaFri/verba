#!/bin/sh
# Crasher fixture: announces success, then kills itself with a signal.
#
# Exercises verba's signal-delivery stop (the signal is re-injected), the
# signaled-termination path, and the discourse-mismatch "killed by signal N"
# deed: the output says success while the command dies by signal.

echo "All done successfully."
kill -TERM $$
