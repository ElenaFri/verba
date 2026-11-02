#ifndef VERBA_UTIL_H
#define VERBA_UTIL_H

#include <stddef.h>

/* Allocation wrappers that abort on OOM — verba has nothing useful to do if a
 * small bookkeeping allocation fails mid-trace. */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* readlink(2) into a fresh buffer; NULL on error. */
char *xreadlink(const char *path);

/* Join dir + rel into a fresh absolute-ish path (handles a trailing slash). */
char *path_join(const char *dir, const char *rel);

/* Lexically normalise an absolute path: collapse duplicate slashes, drop ".",
 * resolve "..", strip a trailing slash. Does not touch the filesystem. */
char *path_lexical(const char *abs);

/* Canonicalise: realpath(3) when the path exists (resolves symlinks), otherwise
 * fall back to path_lexical(). Input should be absolute. */
char *path_canon(const char *abs);

#endif /* VERBA_UTIL_H */
