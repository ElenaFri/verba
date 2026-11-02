#include "reconcile.h"
#include "util.h"
#include "words.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tiny string set (linear; trace volumes are small) ---- */

static int set_has(const struct vec *s, const char *str)
{
	for (size_t i = 0; i < s->len; i++)
		if (strcmp(s->data[i], str) == 0)
			return 1;
	return 0;
}

static void set_add(struct vec *s, const char *str)
{
	if (!set_has(s, str))
		vec_push(s, xstrdup(str));
}

static void free_str(void *p)
{
	free(p);
}

/* The system temp directory: $TMPDIR when set to an absolute path, else the
 * libc default (P_tmpdir). Its contents are ephemeral, so writes there are
 * never treated as undeclared. */
static int under_tmpdir(const char *path)
{
	const char *tmp = getenv("TMPDIR");
	if (!tmp || tmp[0] != '/')
		tmp = P_tmpdir;

	size_t n = strlen(tmp);
	while (n > 0 && tmp[n - 1] == '/')
		n--;
	if (strncmp(path, tmp, n) != 0)
		return 0;
	return path[n] == '/' || path[n] == '\0';
}

/* Built-in undeclared-write whitelist plus any user --allow prefixes. */
static int whitelisted(const char *path, char *const *allow, int nallow)
{
	/* Pseudo- and device filesystems whose writes are never "declared". */
	static const char *const builtin[] = {
		"/dev/", "/proc/", "/sys/", "/run/", NULL,
	};

	if (path[0] != '/') /* pipes, sockets, anonymous fds */
		return 1;

	for (int i = 0; builtin[i]; i++)
		if (strncmp(path, builtin[i], strlen(builtin[i])) == 0)
			return 1;

	if (under_tmpdir(path))
		return 1;

	for (int i = 0; i < nallow; i++)
		if (allow[i] && strncmp(path, allow[i], strlen(allow[i])) == 0)
			return 1;

	return 0;
}

static void add_finding(struct vec *findings, enum finding_type type,
                        const char *path, const char *word, const char *deed,
                        const char *detail)
{
	struct finding *f = xmalloc(sizeof *f);
	f->type = type;
	f->path = path ? xstrdup(path) : NULL;
	f->evidence_word = word ? xstrdup(word) : NULL;
	f->evidence_deed = deed ? xstrdup(deed) : NULL;
	f->detail = detail ? xstrdup(detail) : NULL;
	vec_push(findings, f);
}

void reconcile(const struct audit *au, struct vec *findings,
               char *const *allow, int nallow)
{
	struct vec opened_write = {0}; /* files opened for writing (success) */
	struct vec modified = {0};     /* files created/modified/removed */
	struct vec mentioned = {0};    /* every path named in the output */

	/* ---- deeds ---- */
	for (size_t i = 0; i < au->events.len; i++) {
		const struct file_event *e = au->events.data[i];
		if (e->err != 0 || !e->path)
			continue;
		switch (e->action) {
		case ACT_OPEN_WRITE:
			set_add(&opened_write, e->path);
			break;
		case ACT_CREATE:
		case ACT_TRUNCATE:
		case ACT_WRITE:
			set_add(&opened_write, e->path);
			set_add(&modified, e->path);
			break;
		case ACT_UNLINK:
		case ACT_RENAME:
			set_add(&modified, e->path);
			break;
		}
	}

	/* ---- words ---- */
	for (size_t i = 0; i < au->lines.len; i++) {
		const struct out_line *l = au->lines.data[i];
		words_extract_paths(l->text, &mentioned);
	}

	/* ---- check 1: faithless claim ---- */
	struct vec reported = {0};
	for (size_t i = 0; i < au->lines.len; i++) {
		const struct out_line *l = au->lines.data[i];
		if (!words_is_write_claim(l->text))
			continue;

		struct vec paths = {0};
		words_extract_paths(l->text, &paths);
		for (size_t j = 0; j < paths.len; j++) {
			const char *p = paths.data[j];
			if (set_has(&opened_write, p) || set_has(&reported, p))
				continue;
			char deed[256];
			snprintf(deed, sizeof deed,
			         "no traced process opened %s for writing", p);
			add_finding(findings, F_FAITHLESS_CLAIM, p, l->text, deed,
			            "output claims a write that never reached the kernel");
			set_add(&reported, p);
		}
		vec_free(&paths, free_str);
	}

	/* ---- check 2: undeclared write ---- */
	struct vec ureported = {0};
	for (size_t i = 0; i < modified.len; i++) {
		const char *m = modified.data[i];
		if (set_has(&mentioned, m) || set_has(&ureported, m))
			continue;
		if (whitelisted(m, allow, nallow))
			continue;

		const char *sysname = "write";
		for (size_t j = 0; j < au->events.len; j++) {
			const struct file_event *e = au->events.data[j];
			if (e->err == 0 && e->path && strcmp(e->path, m) == 0 &&
			    e->action != ACT_OPEN_WRITE) {
				sysname = e->sysname;
				break;
			}
		}
		char deed[256];
		snprintf(deed, sizeof deed, "%s %s", sysname, m);
		add_finding(findings, F_UNDECLARED_WRITE, m, NULL, deed,
		            "a file was created or modified but no output mentions it");
		set_add(&ureported, m);
	}

	/* ---- check 3: discourse mismatch ---- */
	int failed = (au->signaled || (au->exited && au->exit_code != 0));
	int succeeded = (au->exited && au->exit_code == 0 && !au->signaled);

	const struct out_line *success_line = NULL, *error_line = NULL;
	for (size_t i = 0; i < au->lines.len; i++) {
		const struct out_line *l = au->lines.data[i];
		if (!success_line && words_is_success(l->text))
			success_line = l;
		if (!error_line && words_is_error(l->text))
			error_line = l;
	}

	if (failed && success_line) {
		char deed[128];
		if (au->signaled)
			snprintf(deed, sizeof deed, "killed by signal %d",
			         au->term_signal);
		else
			snprintf(deed, sizeof deed, "exited with status %d",
			         au->exit_code);
		add_finding(findings, F_DISCOURSE_MISMATCH, NULL,
		            success_line->text, deed,
		            "output reports success but the command failed");
	}
	if (succeeded && error_line) {
		add_finding(findings, F_DISCOURSE_MISMATCH, NULL, error_line->text,
		            "exited with status 0",
		            "output reports an error but the command succeeded");
	}

	vec_free(&opened_write, free_str);
	vec_free(&modified, free_str);
	vec_free(&mentioned, free_str);
	vec_free(&reported, free_str);
	vec_free(&ureported, free_str);
}
