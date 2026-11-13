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
#include "patterns.h"
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
		"      --tty            run the command on a pty (stdout and stderr are\n"
		"                       then captured as one combined stream) — for\n"
		"                       programs that change what they say off-terminal\n"
		"      --follow PID     attach to an already running process instead of\n"
		"                       launching a command; its output is read from the\n"
		"                       write(2) calls it makes to stdout and stderr\n"
		"      --allow PREFIX   treat writes under PREFIX as declared "
		"(repeatable)\n"
		"      --heuristic      also run phase-2 mistranslation checks "
		"(errno vs. claim),\n"
		"                       reported separately; does not affect the "
		"exit code\n"
		"      --strict         run the heuristics and count them toward "
		"the exit code\n"
		"      --patterns FILE   load extra errno<->phrase patterns from FILE (a\n"
		"                       JSON array of {\"phrase\", \"errno\"} objects),\n"
		"                       used by the mistranslation checks (repeatable)\n"
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
	int tty; /* capture through a pty instead of pipes */
	pid_t follow; /* attach to this PID instead of launching a command */
	const char *output;
	char **allow;
	int nallow;
	int heuristic; /* run phase-2 checks, report them separately */
	int strict;    /* run phase-2 checks and fail (exit 1) on them too */
	char **patterns; /* --patterns files, loaded before the trace */
	int npatterns;
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

/* Append one element to a dynamically grown argv-style vector. */
static void append_str(char ***vec, int *n, char *s)
{
	*vec = xrealloc(*vec, (size_t)(*n + 1) * sizeof(**vec));
	(*vec)[(*n)++] = s;
}

/* Parse the --follow argument at argv[*i + 1] as a positive PID. On success
 * stores it, advances *i past both words and returns 0; on error returns -1
 * after a diagnostic. */
static int parse_follow(int argc, char **argv, int *i, struct options *opt)
{
	const char *pa = option_arg(argc, argv, *i, "a PID argument");
	if (!pa)
		return -1;
	errno = 0;
	char *end;
	long v = strtol(pa, &end, 10);
	if (errno || end == pa || *end != '\0' || v <= 0) {
		fprintf(stderr, "%s: --follow needs a positive PID\n", progname);
		hint_help();
		return -1;
	}
	opt->follow = (pid_t)v;
	*i += 2;
	return 0;
}

enum opt_status {
	OPT_CONSUMED, /* option handled; keep scanning */
	OPT_ENDOPTS,  /* "--" seen; the command begins next */
	OPT_DONE,     /* --help/--version handled; exit successfully */
	OPT_ERROR,    /* usage error already reported */
};

/* Handle the single option at argv[*i], advancing *i past it (and its value). */
static enum opt_status parse_one(int argc, char **argv, int *i,
                                 struct options *opt)
{
	const char *arg = argv[*i];

	if (strcmp(arg, "--") == 0) {
		(*i)++;
		return OPT_ENDOPTS;
	}
	if (opt_is(arg, "-h", "--help")) {
		print_usage(stdout);
		return OPT_DONE;
	}
	if (opt_is(arg, "-V", "--version")) {
		print_version();
		return OPT_DONE;
	}
	if (strcmp(arg, "--json") == 0) {
		opt->json = 1;
		(*i)++;
		return OPT_CONSUMED;
	}
	if (strcmp(arg, "--tty") == 0) {
		opt->tty = 1;
		(*i)++;
		return OPT_CONSUMED;
	}
	if (strcmp(arg, "--follow") == 0)
		return parse_follow(argc, argv, i, opt) < 0 ? OPT_ERROR
		                                             : OPT_CONSUMED;
	if (strcmp(arg, "--heuristic") == 0) {
		opt->heuristic = 1;
		(*i)++;
		return OPT_CONSUMED;
	}
	if (strcmp(arg, "--strict") == 0) {
		opt->strict = 1;
		(*i)++;
		return OPT_CONSUMED;
	}
	if (opt_is(arg, "-o", "--output")) {
		opt->output = option_arg(argc, argv, *i, "a FILE argument");
		if (!opt->output)
			return OPT_ERROR;
		*i += 2;
		return OPT_CONSUMED;
	}
	if (strcmp(arg, "--patterns") == 0) {
		char *pf = option_arg(argc, argv, *i, "a FILE argument");
		if (!pf)
			return OPT_ERROR;
		append_str(&opt->patterns, &opt->npatterns, pf);
		*i += 2;
		return OPT_CONSUMED;
	}
	if (strcmp(arg, "--allow") == 0) {
		char *pfx = option_arg(argc, argv, *i, "a PREFIX argument");
		if (!pfx)
			return OPT_ERROR;
		append_str(&opt->allow, &opt->nallow, pfx);
		*i += 2;
		return OPT_CONSUMED;
	}

	fprintf(stderr, "%s: unknown option '%s'\n", progname, arg);
	hint_help();
	return OPT_ERROR;
}

static enum parse_result parse_args(int argc, char **argv, struct options *opt)
{
	int i = 1;
	while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
		enum opt_status st = parse_one(argc, argv, &i, opt);
		if (st == OPT_ENDOPTS)
			break;
		if (st == OPT_DONE)
			return PARSE_DONE;
		if (st == OPT_ERROR)
			return PARSE_ERROR;
	}

	opt->cmd_index = i;
	return PARSE_RUN;
}

/* Validate the parsed option/command combination. Returns a message (for
 * stderr), or NULL when the run may proceed. */
static const char *bad_combo(const struct options *opt, int argc)
{
	if (opt->cmd_index >= argc) {
		if (!opt->follow)
			return "no command given";
	} else if (opt->follow) {
		return "--follow does not take a command";
	}
	if (opt->follow && opt->tty)
		return "--follow cannot be combined with --tty";
	return NULL;
}

int main(int argc, char **argv)
{
	struct options opt;
	memset(&opt, 0, sizeof opt);

	enum parse_result pr = parse_args(argc, argv, &opt);
	if (pr == PARSE_DONE) {
		free(opt.allow);
		free(opt.patterns);
		return EXIT_FAITHFUL;
	}
	if (pr == PARSE_ERROR) {
		free(opt.allow);
		free(opt.patterns);
		return EXIT_ERROR;
	}

	const char *combo_err = bad_combo(&opt, argc);
	if (combo_err) {
		fprintf(stderr, "%s: %s\n", progname, combo_err);
		hint_help();
		free(opt.allow);
		free(opt.patterns);
		return EXIT_ERROR;
	}

	char **cmd = &argv[opt.cmd_index];

	/* Load the pattern dictionaries up front: a bad file is a usage error,
	 * best reported before the command runs. */
	for (int i = 0; i < opt.npatterns; i++) {
		char err[256];
		if (patterns_load(opt.patterns[i], err, sizeof err) < 0) {
			fprintf(stderr, "%s: %s\n", progname, err);
			patterns_reset();
			free(opt.allow);
			free(opt.patterns);
			return EXIT_ERROR;
		}
	}

	struct audit au;
	memset(&au, 0, sizeof au);

	if (trace_run(cmd, &au, opt.tty, opt.follow) < 0) {
		if (opt.follow)
			fprintf(stderr, "%s: cannot attach to pid %ld: %s\n",
			        progname, (long)opt.follow, strerror(errno));
		else
			/* LCOV_EXCL_START: launch-path internal failure is defensive. */
			fprintf(stderr, "%s: could not start tracing: %s\n",
			        progname, strerror(errno));
			/* LCOV_EXCL_STOP */
		audit_free(&au);
		patterns_reset();
		free(opt.allow);
		free(opt.patterns);
		return EXIT_ERROR;
	}
	if (au.exec_failed) {
		fprintf(stderr, "%s: cannot run '%s': %s\n", progname, cmd[0],
		        strerror(au.exec_errno));
		audit_free(&au);
		patterns_reset();
		free(opt.allow);
		free(opt.patterns);
		return EXIT_ERROR;
	}

	/* Attach mode has no argv to report; label the audit with the PID. */
	char pidlabel[32];
	char *attach_argv[2] = {pidlabel, NULL};
	if (opt.follow) {
		snprintf(pidlabel, sizeof pidlabel, "pid:%ld", (long)opt.follow);
		au.argv = attach_argv;
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
			patterns_reset();
			free(opt.allow);
			free(opt.patterns);
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
	patterns_reset();
	free(opt.allow);
	free(opt.patterns);
	return code;
}
