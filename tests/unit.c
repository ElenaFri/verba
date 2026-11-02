/*
 * verba unit tests — the pure-logic modules (no ptrace required).
 *
 * Linked against the same instrumented objects as ./verba, so running this
 * binary contributes to the coverage of util/words/model/reconcile/report/vec.
 * Exercises the branches that the fixture corpus cannot reach from the outside.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "heuristic.h"
#include "model.h"
#include "patterns.h"
#include "reconcile.h"
#include "report.h"
#include "util.h"
#include "vec.h"
#include "words.h"

static int checks;
static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		checks++;                                                      \
		if (!(cond)) {                                                \
			failures++;                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
			        __LINE__, #cond);                            \
		}                                                            \
	} while (0)

#define CHECK_STR(got, want)                                                   \
	do {                                                                   \
		checks++;                                                      \
		if (strcmp((got), (want)) != 0) {                            \
			failures++;                                          \
			fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n",    \
			        __FILE__, __LINE__, (got), (want));          \
		}                                                            \
	} while (0)

/* ---- util.c ---- */

static void test_util(void)
{
	char *s = xstrdup("hello");
	CHECK_STR(s, "hello");
	free(s);

	/* xstrndup: n shorter than, equal to, and longer than the string. */
	s = xstrndup("hello", 3);
	CHECK_STR(s, "hel");
	free(s);
	s = xstrndup("hi", 10);
	CHECK_STR(s, "hi");
	free(s);

	/* xmalloc(0)/xrealloc edge sizes must still return usable storage. */
	void *p = xmalloc(0);
	CHECK(p != NULL);
	p = xrealloc(p, 8);
	CHECK(p != NULL);
	p = xrealloc(p, 0);
	CHECK(p != NULL);
	free(p);

	/* path_join: with and without a trailing slash on the dir. */
	char *j = path_join("/a/b", "c");
	CHECK_STR(j, "/a/b/c");
	free(j);
	j = path_join("/a/b/", "c");
	CHECK_STR(j, "/a/b/c");
	free(j);

	/* path_lexical: the normalisation corpus. */
	struct {
		const char *in;
		const char *want;
	} lex[] = {
		{"/a/b/c", "/a/b/c"},   {"/a/./b", "/a/b"},
		{"/a/../b", "/b"},      {"/", "/"},
		{"", "/"},              {"/a/", "/a"},
		{"a/b", "/a/b"},        {"/..", "/"},
		{"/a/b/../..", "/"},    {"/a//b///c", "/a/b/c"},
		{"/../../a", "/a"},     {"/.", "/"},
		{"/...", "/..."},       {"/a/.hidden", "/a/.hidden"},
	};
	for (size_t i = 0; i < sizeof lex / sizeof lex[0]; i++) {
		char *out = path_lexical(lex[i].in);
		CHECK_STR(out, lex[i].want);
		free(out);
	}

	/* path_canon: an existing path resolves via realpath; a missing one
	 * falls back to the lexical normalisation. */
	char *c = path_canon("/");
	CHECK_STR(c, "/");
	free(c);
	c = path_canon("/no/such/verba/xyz/../xyz");
	CHECK_STR(c, "/no/such/verba/xyz");
	free(c);
}

static void test_xreadlink(void)
{
	char tmpl[] = "verba-ut-XXXXXX";
	/* mkdtemp under cwd (the test runs from the project root). */
	char *dir = mkdtemp(tmpl);
	CHECK(dir != NULL);
	if (!dir)
		return;

	char link[256];
	char target[256];
	snprintf(link, sizeof link, "%s/lnk", dir);
	snprintf(target, sizeof target, "/some/target/path");
	CHECK(symlink(target, link) == 0);

	char *got = xreadlink(link);
	CHECK(got != NULL);
	if (got)
		CHECK_STR(got, target);
	free(got);

	/* A target longer than the initial 256-byte buffer forces xreadlink to
	 * grow its buffer and retry. */
	char longtarget[600];
	memset(longtarget, 'z', sizeof longtarget - 1);
	longtarget[0] = '/';
	longtarget[sizeof longtarget - 1] = '\0';
	char link2[256];
	snprintf(link2, sizeof link2, "%s/lnk2", dir);
	CHECK(symlink(longtarget, link2) == 0);
	char *big = xreadlink(link2);
	CHECK(big != NULL);
	if (big)
		CHECK_STR(big, longtarget);
	free(big);

	/* readlink on a non-symlink fails -> NULL. */
	char *none = xreadlink(dir);
	CHECK(none == NULL);
	free(none);

	unlink(link);
	unlink(link2);
	rmdir(dir);
}

/* ---- words.c ---- */

static void test_words(void)
{
	CHECK(words_is_write_claim("Saved the file"));
	CHECK(words_is_write_claim("now WRITING output"));
	CHECK(!words_is_write_claim("nothing happened here"));
	/* word boundary: "created" matches, but a needle must not fire inside
	 * a larger word. */
	CHECK(!words_is_write_claim("uncreatedness")); /* no standalone verb */

	CHECK(words_is_success("Done."));
	CHECK(words_is_success("it is OK"));
	CHECK(!words_is_success("brokenness")); /* "ok" inside a word */

	CHECK(words_is_error("Error: boom"));
	CHECK(words_is_error("file not found")); /* multi-word phrase */
	CHECK(!words_is_error("everything fine"));
}

static int vec_has_suffix(const struct vec *v, const char *suffix)
{
	size_t sl = strlen(suffix);
	for (size_t i = 0; i < v->len; i++) {
		const char *s = v->data[i];
		size_t l = strlen(s);
		if (l >= sl && strcmp(s + l - sl, suffix) == 0)
			return 1;
	}
	return 0;
}

static void free_str(void *p)
{
	free(p);
}

static void test_extract_paths(void)
{
	setenv("HOME", "/home/verbatest", 1);

	struct vec v = {0};
	words_extract_paths(
		"wrote /no/such/abs/file and ./rel/one and ../rel/two "
		"and ~/tilde and $HOME/dollar and bare/slash/word "
		"(skip http://example.com/u) 'quoted/path'.",
		&v);

	CHECK(vec_has_suffix(&v, "/no/such/abs/file"));
	CHECK(vec_has_suffix(&v, "/rel/one"));
	CHECK(vec_has_suffix(&v, "/rel/two"));
	CHECK(vec_has_suffix(&v, "/home/verbatest/tilde"));
	CHECK(vec_has_suffix(&v, "/home/verbatest/dollar"));
	CHECK(vec_has_suffix(&v, "bare/slash/word"));
	CHECK(vec_has_suffix(&v, "quoted/path"));
	/* URLs are never treated as paths. */
	for (size_t i = 0; i < v.len; i++)
		CHECK(strstr(v.data[i], "example.com") == NULL);

	vec_free(&v, free_str);

	/* bare "~" and "$HOME" with no trailing component still expand. */
	struct vec v2 = {0};
	words_extract_paths("home is ~ exactly", &v2);
	CHECK(vec_has_suffix(&v2, "/home/verbatest"));
	vec_free(&v2, free_str);

	/* a line with no path-like tokens yields nothing. */
	struct vec v3 = {0};
	words_extract_paths("just some words without slashes", &v3);
	CHECK(v3.len == 0);
	vec_free(&v3, free_str);
}

/* ---- model.c ---- */

static void test_model_names(void)
{
	CHECK_STR(file_action_name(ACT_OPEN_WRITE), "open_write");
	CHECK_STR(file_action_name(ACT_CREATE), "create");
	CHECK_STR(file_action_name(ACT_TRUNCATE), "truncate");
	CHECK_STR(file_action_name(ACT_WRITE), "write");
	CHECK_STR(file_action_name(ACT_UNLINK), "unlink");
	CHECK_STR(file_action_name(ACT_RENAME), "rename");
	CHECK_STR(file_action_name((enum file_action)999), "?");

	CHECK_STR(finding_type_name(F_FAITHLESS_CLAIM), "faithless_claim");
	CHECK_STR(finding_type_name(F_UNDECLARED_WRITE), "undeclared_write");
	CHECK_STR(finding_type_name(F_DISCOURSE_MISMATCH), "discourse_mismatch");
	CHECK_STR(finding_type_name(F_MISTRANSLATION), "mistranslation");
	CHECK_STR(finding_type_name((enum finding_type)999), "?");
}

/* ---- builders for reconcile/report fixtures ---- */

static void push_line(struct audit *au, int stream, const char *text)
{
	struct out_line *l = xmalloc(sizeof *l);
	l->order = au->lines.len;
	l->stream = stream;
	l->text = xstrdup(text);
	vec_push(&au->lines, l);
}

static void push_evt(struct audit *au, enum file_action a, const char *path,
                     const char *path2, int err, const char *sysname)
{
	struct file_event *e = xmalloc(sizeof *e);
	e->pid = 1234;
	e->action = a;
	e->path = path ? xstrdup(path) : NULL;
	e->path2 = path2 ? xstrdup(path2) : NULL;
	e->result = err ? -err : 0;
	e->err = err;
	e->sysname = sysname;
	vec_push(&au->events, e);
}

static int has_finding(const struct vec *f, enum finding_type t)
{
	for (size_t i = 0; i < f->len; i++) {
		const struct finding *x = f->data[i];
		if (x->type == t)
			return 1;
	}
	return 0;
}

/* ---- reconcile.c ---- */

static void test_reconcile_faithless(void)
{
	struct audit au = {0};
	push_line(&au, 1, "Saved /no/such/verba/out.conf");
	struct vec f = {0};
	reconcile(&au, &f, NULL, 0);
	CHECK(has_finding(&f, F_FAITHLESS_CLAIM));
	findings_free(&f);
	audit_free(&au);
}

static void test_reconcile_undeclared_and_whitelist(void)
{
	/* A genuine undeclared write is reported. */
	struct audit au = {0};
	push_evt(&au, ACT_CREATE, "/no/such/verba/made.txt", NULL, 0, "mkdir");
	struct vec f = {0};
	reconcile(&au, &f, NULL, 0);
	CHECK(has_finding(&f, F_UNDECLARED_WRITE));
	findings_free(&f);
	audit_free(&au);

	/* Built-in whitelist: /proc, non-absolute, and $TMPDIR are suppressed. */
	setenv("TMPDIR", "/no/such/verba/tmp/", 1); /* trailing slash stripped */
	struct audit au2 = {0};
	push_evt(&au2, ACT_WRITE, "/proc/42/status", NULL, 0, "write");
	push_evt(&au2, ACT_WRITE, "pipe:[12345]", NULL, 0, "write");
	push_evt(&au2, ACT_CREATE, "/no/such/verba/tmp/scratch", NULL, 0,
	         "mkdir");
	push_evt(&au2, ACT_UNLINK, "/no/such/verba/gone", NULL, 0, "unlink");
	struct vec f2 = {0};
	/* --allow suppresses the unlink under the given prefix. */
	char *allow[] = {(char *)"/no/such/verba/gone"};
	reconcile(&au2, &f2, allow, 1);
	CHECK(!has_finding(&f2, F_UNDECLARED_WRITE));
	findings_free(&f2);
	audit_free(&au2);
	unsetenv("TMPDIR");

	/* Errored syscalls and a mention both suppress the finding. */
	struct audit au3 = {0};
	push_evt(&au3, ACT_CREATE, "/no/such/verba/failed", NULL, 13, "mkdir");
	push_evt(&au3, ACT_RENAME, "/no/such/verba/named", "/old", 0, "rename");
	push_line(&au3, 1, "renamed to /no/such/verba/named");
	struct vec f3 = {0};
	reconcile(&au3, &f3, NULL, 0);
	CHECK(!has_finding(&f3, F_UNDECLARED_WRITE));
	findings_free(&f3);
	audit_free(&au3);
}

static void test_reconcile_discourse(void)
{
	/* success text + nonzero exit. */
	struct audit au = {0};
	au.exited = 1;
	au.exit_code = 1;
	push_line(&au, 1, "Installation completed successfully.");
	struct vec f = {0};
	reconcile(&au, &f, NULL, 0);
	CHECK(has_finding(&f, F_DISCOURSE_MISMATCH));
	findings_free(&f);
	audit_free(&au);

	/* error text + zero exit. */
	struct audit au2 = {0};
	au2.exited = 1;
	au2.exit_code = 0;
	push_line(&au2, 2, "fatal: it failed");
	struct vec f2 = {0};
	reconcile(&au2, &f2, NULL, 0);
	CHECK(has_finding(&f2, F_DISCOURSE_MISMATCH));
	findings_free(&f2);
	audit_free(&au2);

	/* killed by a signal + success text (the "killed by signal" deed). */
	struct audit au3 = {0};
	au3.signaled = 1;
	au3.term_signal = 9;
	push_line(&au3, 1, "all done");
	struct vec f3 = {0};
	reconcile(&au3, &f3, NULL, 0);
	CHECK(has_finding(&f3, F_DISCOURSE_MISMATCH));
	findings_free(&f3);
	audit_free(&au3);
}

/* ---- patterns.c ---- */

static void test_patterns(void)
{
	/* names: a known code resolves, an unknown one is NULL. */
	CHECK(errno_name(ENOENT) != NULL);
	if (errno_name(ENOENT))
		CHECK_STR(errno_name(ENOENT), "ENOENT");
	CHECK(errno_name(-12345) == NULL);

	/* families: EACCES and EPERM collapse together; ENOENT stands apart. */
	CHECK(errno_family(EACCES) == errno_family(EPERM));
	CHECK(errno_family(ENOENT) != errno_family(EACCES));

	int codes[8];
	/* one phrase -> one code. */
	CHECK(errno_phrases_in_line("error: no such file", codes, 8) == 1);
	/* two same-family phrases dedupe to one. */
	CHECK(errno_phrases_in_line("permission denied, access denied", codes,
	                            8) == 1);
	/* two distinct families -> two codes. */
	CHECK(errno_phrases_in_line("no such file and permission denied", codes,
	                            8) == 2);
	/* nothing recognisable -> zero. */
	CHECK(errno_phrases_in_line("everything is fine here", codes, 8) == 0);
}

/* ---- heuristic.c ---- */

static void test_heuristic(void)
{
	/* (a) error wording blames the wrong errno: EACCES said, ENOENT real. */
	struct audit au = {0};
	push_line(&au, 2,
	          "cannot create /no/such/verba/out.txt: permission denied");
	push_evt(&au, ACT_OPEN_WRITE, "/no/such/verba/out.txt", NULL, ENOENT,
	         "openat");
	struct vec f = {0};
	reconcile_heuristic(&au, &f);
	CHECK(f.len == 1);
	CHECK(has_finding(&f, F_MISTRANSLATION));
	if (f.len == 1) {
		const struct finding *x = f.data[0];
		CHECK(x->heuristic == 1);
	}
	findings_free(&f);
	audit_free(&au);

	/* (b) false success: a write claim over a path whose write failed. */
	struct audit au2 = {0};
	push_line(&au2, 1, "Saved results to /no/such/verba/s.txt");
	push_evt(&au2, ACT_OPEN_WRITE, "/no/such/verba/s.txt", NULL, EACCES,
	         "openat");
	struct vec f2 = {0};
	reconcile_heuristic(&au2, &f2);
	CHECK(has_finding(&f2, F_MISTRANSLATION));
	findings_free(&f2);
	audit_free(&au2);

	/* (c) wording agrees with the real errno: no finding. */
	struct audit au3 = {0};
	push_line(&au3, 2, "/no/such/verba/x.txt: no such file");
	push_evt(&au3, ACT_OPEN_WRITE, "/no/such/verba/x.txt", NULL, ENOENT,
	         "openat");
	struct vec f3 = {0};
	reconcile_heuristic(&au3, &f3);
	CHECK(f3.len == 0);
	findings_free(&f3);
	audit_free(&au3);

	/* (d) the failed path is never named in the output: no finding. */
	struct audit au4 = {0};
	push_line(&au4, 2, "something went wrong somewhere");
	push_evt(&au4, ACT_OPEN_WRITE, "/no/such/verba/hidden.txt", NULL, EACCES,
	         "openat");
	struct vec f4 = {0};
	reconcile_heuristic(&au4, &f4);
	CHECK(f4.len == 0);
	findings_free(&f4);
	audit_free(&au4);

	/* (e) a successful event (err == 0) is never a mistranslation. */
	struct audit au5 = {0};
	push_line(&au5, 1, "Saved results to /no/such/verba/ok.txt");
	push_evt(&au5, ACT_OPEN_WRITE, "/no/such/verba/ok.txt", NULL, 0,
	         "openat");
	struct vec f5 = {0};
	reconcile_heuristic(&au5, &f5);
	CHECK(f5.len == 0);
	findings_free(&f5);
	audit_free(&au5);

	/* (f) two failed events on one path -> at most one mistranslation. */
	struct audit au6 = {0};
	push_line(&au6, 2,
	          "cannot create /no/such/verba/dup.txt: permission denied");
	push_evt(&au6, ACT_OPEN_WRITE, "/no/such/verba/dup.txt", NULL, ENOENT,
	         "openat");
	push_evt(&au6, ACT_OPEN_WRITE, "/no/such/verba/dup.txt", NULL, ENOENT,
	         "openat");
	struct vec f6 = {0};
	reconcile_heuristic(&au6, &f6);
	CHECK(f6.len == 1);
	findings_free(&f6);
	audit_free(&au6);

	/* (g) an errno with no known name is labelled "errno N". */
	struct audit au7 = {0};
	push_line(&au7, 1, "Saved results to /no/such/verba/unk.txt");
	push_evt(&au7, ACT_OPEN_WRITE, "/no/such/verba/unk.txt", NULL, 9999,
	         "openat");
	struct vec f7 = {0};
	reconcile_heuristic(&au7, &f7);
	CHECK(f7.len == 1);
	if (f7.len == 1) {
		const struct finding *x = f7.data[0];
		CHECK(strstr(x->evidence_deed, "errno 9999") != NULL);
	}
	findings_free(&f7);
	audit_free(&au7);
}

/* ---- report.c ---- */

static char *capture_human(const struct audit *au, const struct vec *f)
{
	char *buf = NULL;
	size_t sz = 0;
	FILE *m = open_memstream(&buf, &sz);
	report_human(m, au, f);
	fclose(m);
	return buf; /* caller frees */
}

static char *capture_json(const struct audit *au, const struct vec *f)
{
	char *buf = NULL;
	size_t sz = 0;
	FILE *m = open_memstream(&buf, &sz);
	report_json(m, au, f);
	fclose(m);
	return buf;
}

static void add_finding_ut(struct vec *f, enum finding_type t, const char *path,
                           const char *word, const char *deed,
                           const char *detail)
{
	struct finding *x = xmalloc(sizeof *x);
	x->type = t;
	x->heuristic = 0;
	x->path = path ? xstrdup(path) : NULL;
	x->evidence_word = word ? xstrdup(word) : NULL;
	x->evidence_deed = deed ? xstrdup(deed) : NULL;
	x->detail = detail ? xstrdup(detail) : NULL;
	vec_push(f, x);
}

static void test_report_human(void)
{
	/* no findings. */
	struct audit au = {0};
	struct vec f = {0};
	char *out = capture_human(&au, &f);
	CHECK(strstr(out, "no contradictions") != NULL);
	free(out);

	/* one finding (singular), with all evidence fields, child exited. */
	au.exited = 1;
	au.exit_code = 1;
	add_finding_ut(&f, F_FAITHLESS_CLAIM, "/x/y", "I saved /x/y",
	               "no write reached the kernel", "claim without a write");
	out = capture_human(&au, &f);
	CHECK(strstr(out, "1 contradiction between") != NULL);
	CHECK(strstr(out, "[faithless claim] /x/y") != NULL);
	CHECK(strstr(out, "words: \"I saved /x/y\"") != NULL);
	CHECK(strstr(out, "deed:") != NULL);
	CHECK(strstr(out, "exited with status 1") != NULL);
	free(out);

	/* two findings (plural) + a finding with no path/evidence, signaled. */
	add_finding_ut(&f, F_DISCOURSE_MISMATCH, NULL, NULL, NULL, NULL);
	struct audit sig = {0};
	sig.signaled = 1;
	sig.term_signal = 11;
	out = capture_human(&sig, &f);
	CHECK(strstr(out, "2 contradictions") != NULL);
	CHECK(strstr(out, "[discourse mismatch]") != NULL);
	CHECK(strstr(out, "killed by signal 11") != NULL);
	free(out);

	findings_free(&f);

	/* the undeclared-write label, plus the unknown-type fallback ("?"). */
	struct vec f2 = {0};
	add_finding_ut(&f2, F_UNDECLARED_WRITE, "/u/v", NULL, "openat /u/v", NULL);
	add_finding_ut(&f2, (enum finding_type)999, NULL, NULL, NULL, NULL);
	out = capture_human(&au, &f2);
	CHECK(strstr(out, "[undeclared write] /u/v") != NULL);
	CHECK(strstr(out, "[?]") != NULL);
	free(out);
	findings_free(&f2);

	/* mechanical + heuristic: two sections, each with its own heading. */
	struct vec f3 = {0};
	add_finding_ut(&f3, F_FAITHLESS_CLAIM, "/m", NULL, NULL, NULL);
	findings_add(&f3, F_MISTRANSLATION, 1, "/h", "it said EACCES",
	             "openat failed with ENOENT", "wrong errno");
	out = capture_human(&au, &f3);
	CHECK(strstr(out, "contradiction") != NULL);
	CHECK(strstr(out, "heuristic finding") != NULL);
	CHECK(strstr(out, "[mistranslation] /h") != NULL);
	free(out);
	findings_free(&f3);

	/* heuristic only: the "no mechanical contradictions" preface appears. */
	struct vec f4 = {0};
	findings_add(&f4, F_MISTRANSLATION, 1, "/h", NULL, NULL, "wrong errno");
	out = capture_human(&au, &f4);
	CHECK(strstr(out, "no mechanical contradictions") != NULL);
	CHECK(strstr(out, "heuristic finding") != NULL);
	free(out);
	findings_free(&f4);
}

static void test_report_json(void)
{
	struct audit au = {0};
	au.exited = 1;
	au.exit_code = 0;
	static char *argv[] = {(char *)"cmd", (char *)"arg", NULL};
	au.argv = argv;
	push_line(&au, 1, "on stdout");
	push_line(&au, 2, "on stderr");
	/* an event with a rename source (path2) and a control/escape soup. */
	push_evt(&au, ACT_RENAME, "/new\tname", "/old\"name", 0, "rename");

	struct vec f = {0};
	add_finding_ut(&f, F_UNDECLARED_WRITE, "/p",
	               "q \" b \\ nl \n cr \r bs \b ff \f ctrl \x01 end",
	               "deed", "detail");

	char *out = capture_json(&au, &f);
	CHECK(strstr(out, "\"schema\": 2") != NULL);
	CHECK(strstr(out, "\"command\": [\"cmd\", \"arg\"]") != NULL);
	CHECK(strstr(out, "\"heuristic\": false") != NULL);
	CHECK(strstr(out, "\"stream\": \"stdout\"") != NULL);
	CHECK(strstr(out, "\"stream\": \"stderr\"") != NULL);
	CHECK(strstr(out, "\"action\": \"rename\"") != NULL);
	/* escaping: tab, quote, backslash, newline, CR, backspace, form feed,
	 * and a \u00xx control. */
	CHECK(strstr(out, "\\t") != NULL);
	CHECK(strstr(out, "\\\"") != NULL);
	CHECK(strstr(out, "\\\\") != NULL);
	CHECK(strstr(out, "\\n") != NULL);
	CHECK(strstr(out, "\\r") != NULL);
	CHECK(strstr(out, "\\b") != NULL);
	CHECK(strstr(out, "\\f") != NULL);
	CHECK(strstr(out, "\\u0001") != NULL);
	free(out);
	findings_free(&f);
	audit_free(&au);

	/* a heuristic finding carries "heuristic": true. */
	struct audit auh = {0};
	struct vec fh = {0};
	findings_add(&fh, F_MISTRANSLATION, 1, "/h", NULL, "deed", "detail");
	out = capture_json(&auh, &fh);
	CHECK(strstr(out, "\"type\": \"mistranslation\"") != NULL);
	CHECK(strstr(out, "\"heuristic\": true") != NULL);
	free(out);
	findings_free(&fh);
	audit_free(&auh);

	/* the empty-collections path: no argv, no findings/claims/trace. */
	struct audit empty = {0};
	struct vec nf = {0};
	out = capture_json(&empty, &nf);
	CHECK(strstr(out, "\"findings\": []") != NULL);
	CHECK(strstr(out, "\"claims\": []") != NULL);
	CHECK(strstr(out, "\"trace\": []") != NULL);
	free(out);
}

/* ---- vec.c ---- */

static void test_vec(void)
{
	struct vec v = {0};
	for (int i = 0; i < 10; i++)
		vec_push(&v, xstrdup("x"));
	CHECK(v.len == 10);
	vec_free(&v, free_str);
	CHECK(v.data == NULL);
	/* free of a zero-initialised vec is a no-op. */
	struct vec z = {0};
	vec_free(&z, NULL);
}

int main(void)
{
	test_util();
	test_xreadlink();
	test_words();
	test_extract_paths();
	test_model_names();
	test_patterns();
	test_heuristic();
	test_reconcile_faithless();
	test_reconcile_undeclared_and_whitelist();
	test_reconcile_discourse();
	test_report_human();
	test_report_json();
	test_vec();

	printf("unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
