#ifndef VERBA_PATTERNS_H
#define VERBA_PATTERNS_H

#include <stddef.h>

/*
 * The faux-amis dictionary: a curated, in-code map between errno values and
 * the error phrasings programs print about them, plus an optional
 * user-extensible table loaded from a JSON file (--patterns FILE). Kept in the
 * same minimal, explicit spirit as the phase-1 word lists in words.c.
 */

/* Collapse near-synonymous errno values into one family so that, e.g., EACCES
 * and EPERM both count as "a permission problem". Unknown codes map to
 * themselves. */
int errno_family(int code);

/* The canonical name of an errno value ("ENOENT", ...), or NULL if unknown. */
const char *errno_name(int code);

/* Reverse lookup: the errno for a canonical name ("ENOENT"), or 0 if unknown. */
int errno_from_name(const char *name);

/* Scan `line` (case-insensitive) for known error phrasings and write the errno
 * codes they imply into `out` (capacity `max`), deduplicated by family. Returns
 * the number of codes written. Covers the built-in table plus anything loaded
 * through patterns_load(). */
int errno_phrases_in_line(const char *line, int *out, int max);

/* Load extra patterns from a JSON file: an array of objects with the string
 * members "phrase" and "errno" (both required), e.g.
 *   [{"phrase": "droits insuffisants", "errno": "EACCES"}]
 * Returns 0, or -1 with a diagnostic in `err`. */
int patterns_load(const char *path, char *err, size_t errn);

/* Free every pattern loaded so far (the built-in table stays). */
void patterns_reset(void);

#endif /* VERBA_PATTERNS_H */
