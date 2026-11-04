#ifndef VERBA_TRACE_H
#define VERBA_TRACE_H

#include "model.h"

/*
 * Run argv[] under ptrace, capturing its interleaved output into au->lines and
 * its file syscalls into au->events, and recording how it exited.
 *
 * With `tty` the command runs on a pty instead of pipes: stdout and stderr
 * arrive as one combined stream (out_line stream tag 0), which is the honest
 * price of seeing the behavior of programs that check isatty().
 *
 * Returns 0 if the command was launched (inspect au->exec_failed for a failed
 * exec), or -1 on a verba-internal failure before the child could run.
 */
int trace_run(char **argv, struct audit *au, int tty);

#endif /* VERBA_TRACE_H */
