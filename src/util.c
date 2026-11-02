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
	/* strndup copies at most n bytes, stopping at the first NUL, and sizes
	 * the allocation to the result — no over-read of the source. */
	char *p = strndup(s, n);
	if (!p)
		oom();
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
	while (*p) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;

		const char *start = p;
		while (*p != '\0' && *p != '/')
			p++;
		size_t seg = (size_t)(p - start);

		if (seg == 1 && start[0] == '.')
			continue;
		if (seg == 2 && start[0] == '.' && start[1] == '.') {
			while (pos > 0 && out[pos - 1] != '/')
				pos--;
			if (pos > 0)
				pos--; /* drop the separating '/' */
			continue;
		}

		if (pos < cap - 1)
			out[pos++] = '/';
		for (size_t i = 0; i < seg && pos < cap - 1; i++)
			out[pos++] = start[i];
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
