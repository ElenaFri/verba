#ifndef VERBA_RECONCILE_H
#define VERBA_RECONCILE_H

#include "model.h"

/*
 * Compare the audit's words against its deeds and append findings.
 * `allow` is an array of `nallow` path prefixes that suppress undeclared-write
 * findings (in addition to the built-in whitelist).
 */
void reconcile(const struct audit *au, struct vec *findings,
               char *const *allow, int nallow);

#endif /* VERBA_RECONCILE_H */
