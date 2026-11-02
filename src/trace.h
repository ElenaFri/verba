#ifndef VERBA_TRACE_H
#define VERBA_TRACE_H

#include "model.h"

/*
 * Run argv[] under ptrace, capturing its interleaved output into au->lines and
 * its file syscalls into au->events, and recording how it exited.
 *
 * Returns 0 if the command was launched (inspect au->exec_failed for a failed
 * exec), or -1 on a verba-internal failure before the child could run.
 */
int trace_run(char **argv, struct audit *au);

#endif /* VERBA_TRACE_H */
