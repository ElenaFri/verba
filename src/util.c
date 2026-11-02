#include "util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void oom(void)
{
	fputs("verba: out of memory\n", stderr);
	abort();
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		oom();
	return p;
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		oom();
	return q;
}

char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = xmalloc(n);
	memcpy(p, s, n);
	return p;
}

char *xstrndup(const char *s, size_t n)
{
	/* Copy at most n bytes, stopping at the first NUL. The length is measured
	 * up front so the allocation and the copy are provably the same size. */
	size_t len = strnlen(s, n);
	char *p = xmalloc(len + 1);
	memcpy(p, s, len);
	p[len] = '\0';
	return p;
}

char *xreadlink(const char *path)
{
	size_t cap = 256;
	for (;;) {
		char *buf = xmalloc(cap);
		ssize_t n = readlink(path, buf, cap - 1);
		if (n < 0) {
			free(buf);
			return NULL;
		}
		if ((size_t)n < cap - 1) {
			buf[n] = '\0';
			return buf;
		}
		free(buf);
		cap *= 2;
	}
}

char *path_join(const char *dir, const char *rel)
{
	size_t dl = strlen(dir);
	int slash = (dl > 0 && dir[dl - 1] == '/') ? 0 : 1;
	size_t n = dl + (size_t)slash + strlen(rel) + 1;
	char *out = xmalloc(n);
	if (slash)
		snprintf(out, n, "%s/%s", dir, rel);
	else
		snprintf(out, n, "%s%s", dir, rel);
	return out;
}

/* Advance *pp past any run of '/' and then over one path component. On success
 * the component is reported via *start (first byte) and *seg (length), *pp is
 * left just past it, and 1 is returned. Returns 0 at the end of the string. */
static int next_component(const char **pp, const char **start, size_t *seg)
{
	const char *p = *pp;
	while (*p == '/')
		p++;
	if (*p == '\0') {
		*pp = p;
		return 0;
	}
	*start = p;
	while (*p != '\0' && *p != '/')
		p++;
	*seg = (size_t)(p - *start);
	*pp = p;
	return 1;
}

/* Apply "..": drop the last component already written to `out` by scanning back
 * over it and its leading '/'. Returns the new write position. */
static size_t pop_component(const char *out, size_t pos)
{
	while (pos > 0 && out[pos - 1] != '/')
		pos--;
	if (pos > 0)
		pos--; /* drop the separating '/' */
	return pos;
}

char *path_lexical(const char *abs)
{
	/* Single pass, always rooted at '/'. Components are appended straight into
	 * `out`; ".." is applied by scanning back to the previous '/'. Every write
	 * is explicitly bounded by `cap`, which fits the worst case (a leading '/'
	 * plus the input plus a terminator). */
	size_t len = strlen(abs);
	size_t cap = len + 2;
	char *out = xmalloc(cap);
	size_t pos = 0;

	const char *p = abs;
	const char *start = NULL;
	size_t seg = 0;
	while (next_component(&p, &start, &seg)) {
		if (seg == 1 && start[0] == '.')
			continue;
		if (seg == 2 && start[0] == '.' && start[1] == '.') {
			pos = pop_component(out, pos);
			continue;
		}

		if (pos < cap - 1)
			out[pos++] = '/';
		for (size_t i = 0; i < seg; i++) {
			if (pos < cap - 1)
				out[pos++] = start[i];
		}
	}

	if (pos == 0) {
		out[0] = '/';
		pos = 1;
	}
	out[pos] = '\0';
	return out;
}

char *path_canon(const char *abs)
{
	char resolved[PATH_MAX];
	if (realpath(abs, resolved))
		return xstrdup(resolved);
	return path_lexical(abs);
}
