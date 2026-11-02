/*
 * verba — audit what a command says against what it does.
 *
 * Phase 0 scaffolding: this file provides the command-line surface only
 * (--help, --version, argument layout, stable exit codes). The tracer and the
 * reconciliation engine land in phase 1; see TODO.md.
 */

#include <stdio.h>
#include <string.h>

#ifndef VERBA_VERSION
#define VERBA_VERSION "0.0.0-dev"
#endif

/* Stable, CI-facing exit-code contract (see README and TODO.md section 1.5). */
enum {
	EXIT_FAITHFUL = 0, /* ran, no contradictions                 */
	EXIT_FINDINGS = 1, /* ran, contradictions found              */
	EXIT_ERROR    = 2, /* usage error or verba-internal failure  */
};

static const char *const progname = "verba";

static void print_version(void)
{
	printf("%s %s\n", progname, VERBA_VERSION);
}

static void print_usage(FILE *out)
{
	fprintf(out,
		"verba — audit what a command says against what it does.\n"
		"\n"
		"Usage:\n"
		"  verba [options] <command> [args...]\n"
		"\n"
		"Options:\n"
		"  -h, --help       show this help and exit\n"
		"  -V, --version    show version and exit\n"
		"\n"
		"verba runs <command> under ptrace and reports contradictions between\n"
		"what it prints and the file syscalls it actually makes.\n"
		"\n"
		"Exit codes:\n"
		"  0  faithful (no contradictions)\n"
		"  1  contradictions found\n"
		"  2  usage or internal error\n");
}

static void hint_help(void)
{
	fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		/* "--" ends option parsing; "-" alone is a command word. */
		if (strcmp(arg, "--") == 0) {
			i++;
			break;
		}
		if (arg[0] != '-' || arg[1] == '\0')
			break;

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			print_usage(stdout);
			return EXIT_FAITHFUL;
		}
		if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
			print_version();
			return EXIT_FAITHFUL;
		}

		fprintf(stderr, "%s: unknown option '%s'\n", progname, arg);
		hint_help();
		return EXIT_ERROR;
	}

	if (i >= argc) {
		fprintf(stderr, "%s: no command given\n", progname);
		hint_help();
		return EXIT_ERROR;
	}

	/* Phase 1: fork + exec argv[i..] under ptrace and audit it. */
	fprintf(stderr,
		"%s: command auditing is not implemented yet "
		"(phase 1 — see TODO.md)\n",
		progname);
	return EXIT_ERROR;
}
