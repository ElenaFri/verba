#ifndef VERBA_PATTERNS_H
#define VERBA_PATTERNS_H

/*
 * The faux-amis dictionary: a curated, in-code map between errno values and the
 * error phrasings programs print about them. Phase 2 only; an external,
 * user-extensible pattern file is deferred (see the roadmap). Kept in the same
 * minimal, explicit spirit as the phase-1 word lists in words.c.
 */

/* Collapse near-synonymous errno values into one family so that, e.g., EACCES
 * and EPERM both count as "a permission problem". Unknown codes map to
 * themselves. */
int errno_family(int code);

/* The canonical name of an errno value ("ENOENT", ...), or NULL if unknown. */
const char *errno_name(int code);

/* Scan `line` (case-insensitive) for known error phrasings and write the errno
 * codes they imply into `out` (capacity `max`), deduplicated by family. Returns
 * the number of codes written. */
int errno_phrases_in_line(const char *line, int *out, int max);

#endif /* VERBA_PATTERNS_H */
