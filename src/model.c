#include "model.h"
#include "util.h"

#include <stdlib.h>

const char *file_action_name(enum file_action a)
{
	switch (a) {
	case ACT_OPEN_WRITE:
		return "open_write";
	case ACT_CREATE:
		return "create";
	case ACT_TRUNCATE:
		return "truncate";
	case ACT_WRITE:
		return "write";
	case ACT_UNLINK:
		return "unlink";
	case ACT_RENAME:
		return "rename";
	}
	return "?";
}

const char *finding_type_name(enum finding_type t)
{
	switch (t) {
	case F_FAITHLESS_CLAIM:
		return "faithless_claim";
	case F_UNDECLARED_WRITE:
		return "undeclared_write";
	case F_DISCOURSE_MISMATCH:
		return "discourse_mismatch";
	case F_MISTRANSLATION:
		return "mistranslation";
	}
	return "?";
}

void out_line_free(void *p)
{
	struct out_line *l = p;
	free(l->text);
	free(l);
}

void file_event_free(void *p)
{
	struct file_event *e = p;
	free(e->path);
	free(e->path2);
	free(e);
}

void finding_free(void *p)
{
	struct finding *f = p;
	free(f->path);
	free(f->evidence_word);
	free(f->evidence_deed);
	free(f->detail);
	free(f);
}

void audit_free(struct audit *au)
{
	vec_free(&au->lines, out_line_free);
	vec_free(&au->events, file_event_free);
}

void findings_free(struct vec *findings)
{
	vec_free(findings, finding_free);
}

void findings_add(struct vec *findings, enum finding_type type, int heuristic,
                  const char *path, const char *word, const char *deed,
                  const char *detail)
{
	struct finding *f = xmalloc(sizeof *f);
	f->type = type;
	f->heuristic = heuristic;
	f->path = path ? xstrdup(path) : NULL;
	f->evidence_word = word ? xstrdup(word) : NULL;
	f->evidence_deed = deed ? xstrdup(deed) : NULL;
	f->detail = detail ? xstrdup(detail) : NULL;
	vec_push(findings, f);
}

size_t findings_count(const struct vec *findings, int heuristic)
{
	size_t n = 0;
	for (size_t i = 0; i < findings->len; i++) {
		const struct finding *f = findings->data[i];
		if (f->heuristic == heuristic)
			n++;
	}
	return n;
}
