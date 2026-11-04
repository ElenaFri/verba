#include "patterns.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "vec.h"

/*
 * Phrase -> errno. Multi-word substrings, matched case-insensitively. The table
 * is deliberately conservative: a phrase is here only when it maps to one errno
 * unambiguously. Order matters only for readability; lookups scan the whole
 * table.
 */
static const struct {
	const char *phrase;
	int code;
} phrase_map[] = {
	{"no such file", ENOENT},
	{"not found", ENOENT},
	{"does not exist", ENOENT},
	{"permission denied", EACCES},
	{"access denied", EACCES},
	{"operation not permitted", EPERM},
	{"already exists", EEXIST},
	{"file exists", EEXIST},
	{"not a directory", ENOTDIR},
	{"is a directory", EISDIR},
	{"no space left", ENOSPC},
	{"disk full", ENOSPC},
	{"read-only file system", EROFS},
	{"read-only", EROFS},
	{"too many open files", EMFILE},
	{"file name too long", ENAMETOOLONG},
	{"name too long", ENAMETOOLONG},
	{"directory not empty", ENOTEMPTY},
	{NULL, 0},
};

static const struct {
	int code;
	const char *name;
} name_map[] = {
	{ENOENT, "ENOENT"},
	{EACCES, "EACCES"},
	{EPERM, "EPERM"},
	{EEXIST, "EEXIST"},
	{ENOTDIR, "ENOTDIR"},
	{EISDIR, "EISDIR"},
	{ENOSPC, "ENOSPC"},
	{EROFS, "EROFS"},
	{EMFILE, "EMFILE"},
	{ENFILE, "ENFILE"},
	{ENAMETOOLONG, "ENAMETOOLONG"},
	{ENOTEMPTY, "ENOTEMPTY"},
	{EXDEV, "EXDEV"},
	{ELOOP, "ELOOP"},
	{0, NULL},
};

int errno_family(int code)
{
	/* EACCES and EPERM are both "you may not"; a program that says one while
	 * the kernel returned the other is not mistranslating. */
	if (code == EPERM)
		return EACCES;
	return code;
}

const char *errno_name(int code)
{
	for (int i = 0; name_map[i].name; i++)
		if (name_map[i].code == code)
			return name_map[i].name;
	return NULL;
}

int errno_from_name(const char *name)
{
	for (int i = 0; name_map[i].name; i++)
		if (strcmp(name_map[i].name, name) == 0)
			return name_map[i].code;
	return 0; /* unknown; 0 is never a valid errno in our tables */
}

/* ---- the user-extensible table (--patterns FILE) ---- */

struct pat {
	char *phrase;
	int code;
};

static struct vec user_pats; /* struct pat *, loaded from --patterns files */

static void pat_free(void *v)
{
	struct pat *q = v;
	free(q->phrase);
	free(q);
}

/* Append `code` unless a code of the same family is already present. */
static int add_family_code(int *out, int max, int n, int code)
{
	int fam = errno_family(code);
	for (int j = 0; j < n; j++)
		if (errno_family(out[j]) == fam)
			return n;
	if (n < max) {
		out[n] = code;
		return n + 1;
	}
	return n;
}

int errno_phrases_in_line(const char *line, int *out, int max)
{
	int n = 0;
	for (int i = 0; phrase_map[i].phrase; i++)
		if (strcasestr(line, phrase_map[i].phrase))
			n = add_family_code(out, max, n, phrase_map[i].code);
	for (size_t i = 0; i < user_pats.len; i++) {
		const struct pat *q = user_pats.data[i];
		if (strcasestr(line, q->phrase))
			n = add_family_code(out, max, n, q->code);
	}
	return n;
}

void patterns_reset(void)
{
	vec_free(&user_pats, pat_free);
}

/* ---- a minimal JSON reader for one fixed schema ----
 *
 * Not a general JSON parser: the file must be an array of objects whose only
 * members are the strings "phrase" and "errno" (both required). The grammar
 * is kept this small on purpose — it stays auditable, and the zero-runtime-
 * dependency rule rules out TOML or a JSON library. A pragmatic superset is
 * accepted (raw control characters inside strings); \uXXXX escapes outside
 * the BMP (surrogate pairs) are rejected with a diagnostic.
 */

struct jparse {
	const char *s;    /* NUL-terminated file contents */
	const char *path; /* for diagnostics */
	size_t pos;
	int line;
};

struct sbuf {
	char *p;
	size_t len;
	size_t cap;
};

static void pfail(const struct jparse *p, const char *msg, char *err,
                  size_t errn)
{
	snprintf(err, errn, "%s:%d: %s", p->path, p->line, msg);
}

static void sb_put(struct sbuf *b, unsigned c)
{
	/* The +4 slack always leaves room for a UTF-8 sequence and the NUL. */
	if (b->len + 4 >= b->cap) {
		b->cap *= 2;
		b->p = xrealloc(b->p, b->cap);
	}
	b->p[b->len++] = (char)c;
}

static void sb_put_utf8(struct sbuf *b, unsigned cp)
{
	if (cp < 0x80) {
		sb_put(b, cp);
	} else if (cp < 0x800) {
		sb_put(b, 0xC0 | (cp >> 6));
		sb_put(b, 0x80 | (cp & 0x3F));
	} else {
		sb_put(b, 0xE0 | (cp >> 12));
		sb_put(b, 0x80 | ((cp >> 6) & 0x3F));
		sb_put(b, 0x80 | (cp & 0x3F));
	}
}

static int hexval(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_hex4(struct jparse *p, unsigned *out)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hexval((unsigned char)p->s[p->pos]);
		if (h < 0)
			return -1;
		v = v * 16 + (unsigned)h;
		p->pos++;
	}
	*out = v;
	return 0;
}

static void skip_ws(struct jparse *p)
{
	for (;;) {
		unsigned char c = (unsigned char)p->s[p->pos];
		if (c == ' ' || c == '\t' || c == '\r') {
			p->pos++;
		} else if (c == '\n') {
			p->pos++;
			p->line++;
		} else {
			break;
		}
	}
}

/* At the opening quote; returns a fresh NUL-terminated string, or NULL with
 * `err` filled. */
static char *parse_string(struct jparse *p, char *err, size_t errn)
{
	struct sbuf b = {xmalloc(64), 0, 64};

	p->pos++; /* opening quote */
	for (;;) {
		unsigned char c = (unsigned char)p->s[p->pos];
		if (c == '\0') {
			free(b.p);
			pfail(p, "unterminated string", err, errn);
			return NULL;
		}
		if (c == '"') {
			p->pos++;
			break;
		}
		if (c == '\\') {
			p->pos++;
			unsigned char e = (unsigned char)p->s[p->pos];
			if (e == '\0') {
				free(b.p);
				pfail(p, "unterminated string", err, errn);
				return NULL;
			}
			if (e == 'u') {
				p->pos++;
				unsigned cp;
				if (parse_hex4(p, &cp) < 0) {
					free(b.p);
					pfail(p, "bad \\u escape", err, errn);
					return NULL;
				}
				if (cp >= 0xD800 && cp <= 0xDFFF) {
					free(b.p);
					pfail(p, "unpaired surrogate escape",
					      err, errn);
					return NULL;
				}
				sb_put_utf8(&b, cp);
				continue;
			}
			switch (e) {
			case '"':
			case '\\':
			case '/':
				c = e;
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			default:
				free(b.p);
				pfail(p, "unknown escape", err, errn);
				return NULL;
			}
			p->pos++;
			sb_put(&b, c);
			continue;
		}
		if (c == '\n')
			p->line++;
		sb_put(&b, c);
		p->pos++;
	}
	b.p[b.len] = '\0';
	return b.p;
}

/* At '{'. On success *phrase (owned) and *code are set; on failure everything
 * is freed and *phrase is NULL. */
static int parse_object(struct jparse *p, char **phrase, int *code, char *err,
                        size_t errn)
{
	*phrase = NULL;
	*code = 0;
	int seen_phrase = 0, seen_errno = 0;

	p->pos++; /* '{' */
	for (;;) {
		skip_ws(p);
		if ((unsigned char)p->s[p->pos] != '"') {
			pfail(p, "expected a key string", err, errn);
			goto fail;
		}
		char *key = parse_string(p, err, errn);
		if (!key)
			goto fail;
		skip_ws(p);
		if (p->s[p->pos] != ':') {
			free(key);
			pfail(p, "expected ':'", err, errn);
			goto fail;
		}
		p->pos++;
		skip_ws(p);
		if ((unsigned char)p->s[p->pos] != '"') {
			free(key);
			pfail(p, "expected a string value", err, errn);
			goto fail;
		}
		char *val = parse_string(p, err, errn);
		if (!val) {
			free(key);
			goto fail;
		}

		if (strcmp(key, "phrase") == 0) {
			if (seen_phrase) {
				free(val);
				free(key);
				pfail(p, "duplicate \"phrase\"", err, errn);
				goto fail;
			}
			if (val[0] == '\0') {
				free(val);
				free(key);
				pfail(p, "empty \"phrase\"", err, errn);
				goto fail;
			}
			*phrase = val;
			seen_phrase = 1;
		} else if (strcmp(key, "errno") == 0) {
			if (seen_errno) {
				free(val);
				free(key);
				pfail(p, "duplicate \"errno\"", err, errn);
				goto fail;
			}
			int c = errno_from_name(val);
			if (c == 0) {
				free(val);
				free(key);
				pfail(p, "unknown errno name", err, errn);
				goto fail;
			}
			*code = c;
			seen_errno = 1;
			free(val);
		} else {
			free(val);
			free(key);
			pfail(p, "unknown key", err, errn);
			goto fail;
		}
		free(key);

		skip_ws(p);
		if (p->s[p->pos] == ',') {
			p->pos++;
			continue;
		}
		if (p->s[p->pos] == '}') {
			p->pos++;
			break;
		}
		pfail(p, "expected ',' or '}'", err, errn);
		goto fail;
	}

	if (!seen_phrase || !seen_errno) {
		pfail(p, "object needs both \"phrase\" and \"errno\"", err, errn);
		goto fail;
	}
	return 0;
fail:
	free(*phrase);
	*phrase = NULL;
	return -1;
}

/* At '['; appends struct pat entries to `pats`. */
static int parse_array(struct jparse *p, struct vec *pats, char *err,
                       size_t errn)
{
	p->pos++; /* '[' */
	skip_ws(p);
	if (p->s[p->pos] == ']') {
		p->pos++;
		return 0;
	}
	for (;;) {
		skip_ws(p);
		if (p->s[p->pos] != '{') {
			pfail(p, "expected '{'", err, errn);
			return -1;
		}
		char *phrase;
		int code;
		if (parse_object(p, &phrase, &code, err, errn) < 0)
			return -1;
		struct pat *q = xmalloc(sizeof *q);
		q->phrase = phrase;
		q->code = code;
		vec_push(pats, q);

		skip_ws(p);
		if (p->s[p->pos] == ',') {
			p->pos++;
			continue;
		}
		if (p->s[p->pos] == ']') {
			p->pos++;
			return 0;
		}
		pfail(p, "expected ',' or ']'", err, errn);
		return -1;
	}
}

/* Whole file into a NUL-terminated buffer, or NULL with `err` filled. */
static char *read_file(const char *path, char *err, size_t errn)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errn, "%s: cannot open: %s", path, strerror(errno));
		return NULL;
	}
	size_t cap = 8192, n = 0;
	char *buf = xmalloc(cap);
	size_t r;
	while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
		n += r;
		if (n == cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
	}
	int bad = ferror(f);
	fclose(f);
	if (bad) {
		free(buf);
		snprintf(err, errn, "%s: read error", path);
		return NULL;
	}
	buf[n] = '\0';
	return buf;
}

int patterns_load(const char *path, char *err, size_t errn)
{
	char *data = read_file(path, err, errn);
	if (!data)
		return -1;

	struct jparse p = {data, path, 0, 1};
	struct vec pats = {0};

	skip_ws(&p);
	if (p.s[p.pos] != '[') {
		pfail(&p, "expected '['", err, errn);
		free(data);
		return -1;
	}
	if (parse_array(&p, &pats, err, errn) < 0) {
		vec_free(&pats, pat_free);
		free(data);
		return -1;
	}
	skip_ws(&p);
	if (p.s[p.pos] != '\0') {
		pfail(&p, "trailing data after ']'", err, errn);
		vec_free(&pats, pat_free);
		free(data);
		return -1;
	}

	/* Everything parsed: splice the entries into the live table. */
	for (size_t i = 0; i < pats.len; i++)
		vec_push(&user_pats, pats.data[i]);
	vec_free(&pats, NULL);
	free(data);
	return 0;
}
