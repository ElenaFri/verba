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
 * With `follow` > 0, argv and tty are ignored: verba attaches to that already
 * running process (and the descendants it spawns from then on) instead of
 * launching a command. Its stdout/stderr cannot be redirected after the fact,
 * so the "words" are reconstructed from the write(2) calls it makes to fds 1
 * and 2. Pre-existing children and threads are not attached.
 *
 * Returns 0 if tracing started (inspect au->exec_failed for a failed exec in
 * the launch case), or -1 on failure before any tracee could run (errno is set;
 * for attach it is the PTRACE_SEIZE error, e.g. ESRCH or EPERM).
 */
int trace_run(char **argv, struct audit *au, int tty, pid_t follow);

#endif /* VERBA_TRACE_H */
