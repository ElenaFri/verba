/*
 * bench_workload — a deterministic, syscall-heavy load for the benchmark.
 *
 * It represents the common case a syscall tracer pays for: a program that makes
 * a great many syscalls, most of which are *not* file operations verba cares
 * about. Each unit of work does a burst of cheap, untraced syscalls (getpid)
 * and, once every FILE_EVERY units, one traced file operation (create, write,
 * unlink). This is exactly the mix where stopping on every syscall hurts.
 *
 * Usage: bench_workload <scratch-dir> [units]
 * It is intentionally free of verba internals; it is only a subject to trace.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define FILE_EVERY 100

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <scratch-dir> [units]\n", argv[0]);
		return 2;
	}

	const char *dir = argv[1];
	long units = (argc > 2) ? strtol(argv[2], NULL, 10) : 200000;
	if (units < 0)
		units = 0;

	char path[4096];
	for (long i = 0; i < units; i++) {
		/* A burst of cheap syscalls the tracer does not care about. */
		for (int k = 0; k < 10; k++)
			(void)syscall(SYS_getpid);

		if (i % FILE_EVERY == 0) {
			snprintf(path, sizeof(path), "%s/bench-%ld.tmp", dir, i);
			int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd >= 0) {
				(void)!write(fd, "x", 1);
				close(fd);
				unlink(path);
			}
		}
	}

	printf("bench_workload: %ld units done\n", units);
	return 0;
}
