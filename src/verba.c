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
#include "heuristic.h"
#include "reconcile.h"
#include "report.h"
#include "trace.h"
#include "util.h"

#ifndef VERBA_VERSION
#define VERBA_VERSION "0.0.0-dev"
#endif

/* Stable, CI-facing exit-code contract (see README and the project roadmap). */
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
		"      --heuristic      also run phase-2 mistranslation checks "
		"(errno vs. claim),\n"
		"                       reported separately; does not affect the "
		"exit code\n"
		"      --strict         run the heuristics and count them toward "
		"the exit code\n"
		"\n"
		"verba runs <command> under ptrace and reports contradictions between\n"
		"what it prints and the file syscalls it actually makes:\n"
		"  faithless claim     output claims a write that never happened\n"
		"  undeclared write    a file changed on disk, unmentioned in output\n"
		"  discourse mismatch  success/error text disagrees with the exit code\n"
		"  mistranslation      error wording disagrees with the real errno "
		"(heuristic)\n"
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

struct options {
	int json;
	const char *output;
	char **allow;
	int nallow;
	int heuristic; /* run phase-2 checks, report them separately */
	int strict;    /* run phase-2 checks and fail (exit 1) on them too */
	int cmd_index; /* index of <command> in argv, or argc if none */
};

enum parse_result {
	PARSE_RUN,   /* options consumed; run argv[cmd_index...] */
	PARSE_DONE,  /* --help/--version handled; exit successfully */
	PARSE_ERROR, /* usage error already reported */
};

/* The argument of an option that takes one, or NULL (after a diagnostic). */
static char *option_arg(int argc, char **argv, int i, const char *what)
{
	if (i + 1 >= argc) {
		fprintf(stderr, "%s: %s requires %s\n", progname, argv[i], what);
		hint_help();
		return NULL;
	}
	return argv[i + 1];
}

/* True if arg matches either the short or the long spelling of an option. */
static int opt_is(const char *arg, const char *short_opt, const char *long_opt)
{
	return strcmp(arg, short_opt) == 0 || strcmp(arg, long_opt) == 0;
}

static enum parse_result parse_args(int argc, char **argv, struct options *opt)
{
	int i = 1;
	while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
		const char *arg = argv[i];

		if (strcmp(arg, "--") == 0) {
			i++;
			break;
		}
		if (opt_is(arg, "-h", "--help")) {
			print_usage(stdout);
			return PARSE_DONE;
		}
		if (opt_is(arg, "-V", "--version")) {
			print_version();
			return PARSE_DONE;
		}
		if (strcmp(arg, "--json") == 0) {
			opt->json = 1;
			i++;
			continue;
		}
		if (strcmp(arg, "--heuristic") == 0) {
			opt->heuristic = 1;
			i++;
			continue;
		}
		if (strcmp(arg, "--strict") == 0) {
			opt->strict = 1;
			i++;
			continue;
		}
		if (opt_is(arg, "-o", "--output")) {
			opt->output = option_arg(argc, argv, i, "a FILE argument");
			if (!opt->output)
				return PARSE_ERROR;
			i += 2;
			continue;
		}
		if (strcmp(arg, "--allow") == 0) {
			char *pfx = option_arg(argc, argv, i, "a PREFIX argument");
			if (!pfx)
				return PARSE_ERROR;
			opt->allow = xrealloc(
				opt->allow,
				(size_t)(opt->nallow + 1) * sizeof(*opt->allow));
			opt->allow[opt->nallow++] = pfx;
			i += 2;
			continue;
		}

		fprintf(stderr, "%s: unknown option '%s'\n", progname, arg);
		hint_help();
		return PARSE_ERROR;
	}

	opt->cmd_index = i;
	return PARSE_RUN;
}

int main(int argc, char **argv)
{
	struct options opt;
	memset(&opt, 0, sizeof opt);

	enum parse_result pr = parse_args(argc, argv, &opt);
	if (pr == PARSE_DONE) {
		free(opt.allow);
		return EXIT_FAITHFUL;
	}
	if (pr == PARSE_ERROR) {
		free(opt.allow);
		return EXIT_ERROR;
	}

	if (opt.cmd_index >= argc) {
		fprintf(stderr, "%s: no command given\n", progname);
		hint_help();
		free(opt.allow);
		return EXIT_ERROR;
	}

	char **cmd = &argv[opt.cmd_index];

	struct audit au;
	memset(&au, 0, sizeof au);

	/* LCOV_EXCL_START: only on pipe/signalfd/fork failure inside trace_run. */
	if (trace_run(cmd, &au) < 0) {
		fprintf(stderr, "%s: could not start tracing: %s\n", progname,
		        strerror(errno));
		audit_free(&au);
		free(opt.allow);
		return EXIT_ERROR;
	}
	/* LCOV_EXCL_STOP */
	if (au.exec_failed) {
		fprintf(stderr, "%s: cannot run '%s': %s\n", progname, cmd[0],
		        strerror(au.exec_errno));
		audit_free(&au);
		free(opt.allow);
		return EXIT_ERROR;
	}

	struct vec findings = {0};
	reconcile(&au, &findings, opt.allow, opt.nallow);
	if (opt.heuristic || opt.strict)
		reconcile_heuristic(&au, &findings);

	if (opt.output) {
		FILE *f = fopen(opt.output, "w");
		if (!f) {
			fprintf(stderr, "%s: cannot open '%s': %s\n", progname,
			        opt.output, strerror(errno));
			findings_free(&findings);
			audit_free(&au);
			free(opt.allow);
			return EXIT_ERROR;
		}
		report_json(f, &au, &findings);
		fclose(f);
		report_human(stdout, &au, &findings);
	} else if (opt.json) {
		report_json(stdout, &au, &findings);
	} else {
		report_human(stdout, &au, &findings);
	}

	size_t mech = findings_count(&findings, 0);
	size_t heur = findings_count(&findings, 1);
	int code = (mech > 0 || (opt.strict && heur > 0)) ? EXIT_FINDINGS
	                                                  : EXIT_FAITHFUL;

	findings_free(&findings);
	audit_free(&au);
	free(opt.allow);
	return code;
}
