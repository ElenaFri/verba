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
	/* Split on '/', keep a stack of components, apply "." and "..". */
	size_t len = strlen(abs);
	char *work = xstrndup(abs, len);

	/* A path of length len has at most len non-empty components. */
	size_t maxcomp = len + 1;
	char **comp = xmalloc(maxcomp * sizeof(*comp));
	size_t ncomp = 0;

	char *save = NULL;
	for (char *tok = strtok_r(work, "/", &save); tok;
	     tok = strtok_r(NULL, "/", &save)) {
		if (strcmp(tok, ".") == 0 || tok[0] == '\0')
			continue;
		if (strcmp(tok, "..") == 0) {
			if (ncomp > 0)
				ncomp--;
			continue;
		}
		if (ncomp < maxcomp)
			comp[ncomp++] = tok;
	}

	/* Rebuild, always rooted at '/'. */
	size_t cap = len + 2;
	char *out = xmalloc(cap);
	out[0] = '\0';
	if (ncomp == 0) {
		strcpy(out, "/");
	} else {
		size_t pos = 0;
		for (size_t i = 0; i < ncomp; i++) {
			out[pos++] = '/';
			size_t cl = strlen(comp[i]);
			memcpy(out + pos, comp[i], cl);
			pos += cl;
		}
		out[pos] = '\0';
	}

	free(comp);
	free(work);
	return out;
}

char *path_canon(const char *abs)
{
	char resolved[PATH_MAX];
	if (realpath(abs, resolved))
		return xstrdup(resolved);
	return path_lexical(abs);
}
