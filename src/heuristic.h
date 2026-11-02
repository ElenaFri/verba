#ifndef VERBA_HEURISTIC_H
#define VERBA_HEURISTIC_H

#include "model.h"

/*
 * Phase-2 mistranslation audit (opt-in, heuristic). For each failed syscall
 * whose path is named in the output, check whether the wording around that path
 * matches the real errno. Appends F_MISTRANSLATION findings, each tagged
 * heuristic. Never run by default; see verba.c (--heuristic / --strict).
 */
void reconcile_heuristic(const struct audit *au, struct vec *findings);

#endif /* VERBA_HEURISTIC_H */
