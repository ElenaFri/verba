/*
 * verba — audit what a command says against what it does.
 *
 * Entry point: parse options, run the command under ptrace (trace.c), extract
 * the paths it names in its output (words.c), reconcile words against deeds
 * (reconcile.c) and report the contradictions (report.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "model.h"
#include "reconcile.h"
#include "report.h"
#include "trace.h"
#include "util.h"

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
		"  -h, --help           show this help and exit\n"
		"  -V, --version        show version and exit\n"
		"      --json           write the JSON report to stdout\n"
		"  -o, --output FILE    write the JSON report to FILE\n"
		"      --allow PREFIX   treat writes under PREFIX as declared "
		"(repeatable)\n"
		"\n"
		"verba runs <command> under ptrace and reports contradictions between\n"
		"what it prints and the file syscalls it actually makes:\n"
		"  faithless claim     output claims a write that never happened\n"
		"  undeclared write    a file changed on disk, unmentioned in output\n"
		"  discourse mismatch  success/error text disagrees with the exit code\n"
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
	int json = 0;
	const char *output = NULL;
	char **allow = NULL;
	int nallow = 0;
	int i;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--") == 0) {
			i++;
			break;
		}
		if (arg[0] != '-' || arg[1] == '\0')
			break;

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			print_usage(stdout);
			free(allow);
			return EXIT_FAITHFUL;
		}
		if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
			print_version();
			free(allow);
			return EXIT_FAITHFUL;
		}
		if (strcmp(arg, "--json") == 0) {
			json = 1;
			continue;
		}
		if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s: %s requires a FILE argument\n",
				        progname, arg);
				hint_help();
				free(allow);
				return EXIT_ERROR;
			}
			output = argv[++i];
			continue;
		}
		if (strcmp(arg, "--allow") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr,
				        "%s: --allow requires a PREFIX argument\n",
				        progname);
				hint_help();
				free(allow);
				return EXIT_ERROR;
			}
			allow = xrealloc(allow, (size_t)(nallow + 1) * sizeof(*allow));
			allow[nallow++] = argv[++i];
			continue;
		}

		fprintf(stderr, "%s: unknown option '%s'\n", progname, arg);
		hint_help();
		free(allow);
		return EXIT_ERROR;
	}

	if (i >= argc) {
		fprintf(stderr, "%s: no command given\n", progname);
		hint_help();
		free(allow);
		return EXIT_ERROR;
	}

	struct audit au;
	memset(&au, 0, sizeof au);

	if (trace_run(&argv[i], &au) < 0) {
		fprintf(stderr, "%s: could not start tracing: %s\n", progname,
		        strerror(errno));
		audit_free(&au);
		free(allow);
		return EXIT_ERROR;
	}
	if (au.exec_failed) {
		fprintf(stderr, "%s: cannot run '%s': %s\n", progname, argv[i],
		        strerror(au.exec_errno));
		audit_free(&au);
		free(allow);
		return EXIT_ERROR;
	}

	struct vec findings = {0};
	reconcile(&au, &findings, allow, nallow);

	if (output) {
		FILE *f = fopen(output, "w");
		if (!f) {
			fprintf(stderr, "%s: cannot open '%s': %s\n", progname,
			        output, strerror(errno));
			findings_free(&findings);
			audit_free(&au);
			free(allow);
			return EXIT_ERROR;
		}
		report_json(f, &au, &findings);
		fclose(f);
		report_human(stdout, &au, &findings);
	} else if (json) {
		report_json(stdout, &au, &findings);
	} else {
		report_human(stdout, &au, &findings);
	}

	int code = findings.len > 0 ? EXIT_FINDINGS : EXIT_FAITHFUL;

	findings_free(&findings);
	audit_free(&au);
	free(allow);
	return code;
}
