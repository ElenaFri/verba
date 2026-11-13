#include "heuristic.h"
#include "patterns.h"
#include "util.h"
#include "words.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CODES 8

static void free_str(void *p)
{
	free(p);
}

/* One F_MISTRANSLATION per path is enough; the first naming line wins. */
static int already_flagged(const struct vec *findings, const char *path)
{
	for (size_t i = 0; i < findings->len; i++) {
		const struct finding *f = findings->data[i];
		if (f->type == F_MISTRANSLATION && f->path &&
		    strcmp(f->path, path) == 0)
			return 1;
	}
	return 0;
}

/* Does this output line name `path` (after the same canonicalisation the
 * mechanical checks use)? Relative paths resolve against `base_cwd`. */
static int line_names_path(const char *line, const char *base_cwd,
                           const char *path)
{
	struct vec paths = {0};
	words_extract_paths(line, base_cwd, &paths);
	int hit = 0;
	for (size_t i = 0; i < paths.len; i++)
		if (strcmp(paths.data[i], path) == 0) {
			hit = 1;
			break;
		}
	vec_free(&paths, free_str);
	return hit;
}

/* Human label for an errno: its name, or "errno N" when we have no name. */
static void label(int code, char *buf, size_t n)
{
	const char *name = errno_name(code);
	if (name)
		snprintf(buf, n, "%s", name);
	else
		snprintf(buf, n, "errno %d", code);
}

/* True when any of the `nc` phrase-implied codes shares the actual errno's
 * family — i.e. the wording and the kernel agree. */
static int wording_agrees(const int *codes, int nc, int actual)
{
	int fam = errno_family(actual);
	for (int i = 0; i < nc; i++)
		if (errno_family(codes[i]) == fam)
			return 1;
	return 0;
}

static void check_event(const struct audit *au, const struct file_event *e,
                        struct vec *findings)
{
	char actual[32];
	label(e->err, actual, sizeof actual);

	for (size_t i = 0; i < au->lines.len; i++) {
		const struct out_line *l = au->lines.data[i];
		if (!line_names_path(l->text, l->cwd, e->path))
			continue;

		int codes[MAX_CODES];
		int nc = errno_phrases_in_line(l->text, codes, MAX_CODES);

		if (nc > 0) {
			if (wording_agrees(codes, nc, e->err))
				continue; /* this line's wording is honest */

			char said[32];
			label(codes[0], said, sizeof said);
			char deed[128];
			char detail[192];
			snprintf(deed, sizeof deed, "%s failed with %s",
			         e->sysname, actual);
			snprintf(detail, sizeof detail,
			         "output reads like %s but the call failed with %s",
			         said, actual);
			findings_add(findings, F_MISTRANSLATION, 1, e->path,
			             l->text, deed, detail);
			return;
		}

		if (words_is_success(l->text) || words_is_write_claim(l->text)) {
			char deed[128];
			snprintf(deed, sizeof deed, "%s failed with %s",
			         e->sysname, actual);
			findings_add(findings, F_MISTRANSLATION, 1, e->path,
			             l->text, deed,
			             "output claims success but the operation failed");
			return;
		}
	}
}

void reconcile_heuristic(const struct audit *au, struct vec *findings)
{
	for (size_t i = 0; i < au->events.len; i++) {
		const struct file_event *e = au->events.data[i];
		if (e->err == 0 || !e->path)
			continue;
		if (already_flagged(findings, e->path))
			continue;
		check_event(au, e, findings);
	}
}
