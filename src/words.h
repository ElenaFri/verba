#ifndef VERBA_WORDS_H
#define VERBA_WORDS_H

#include "vec.h"

/* Pattern probes over a single output line (case-insensitive). */
int words_is_write_claim(const char *line); /* saved / wrote / created / ... */
int words_is_success(const char *line);     /* success / done / completed ... */
int words_is_error(const char *line);       /* error / failed / not found ... */

/* Extract path-like tokens from a line and append each, canonicalised to an
 * absolute path (owned char*), to `out`. Relative words are resolved against
 * verba's cwd (an MVP approximation of "the cwd at the time of the claim").
 * Tilde and $HOME are expanded. */
void words_extract_paths(const char *line, struct vec *out);

#endif /* VERBA_WORDS_H */
