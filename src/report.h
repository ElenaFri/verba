#ifndef VERBA_REPORT_H
#define VERBA_REPORT_H

#include <stdio.h>

#include "model.h"

void report_human(FILE *out, const struct audit *au, const struct vec *findings);
void report_json(FILE *out, const struct audit *au, const struct vec *findings);

#endif /* VERBA_REPORT_H */
