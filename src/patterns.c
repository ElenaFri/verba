#include "patterns.h"

#include <errno.h>
#include <string.h>

/*
 * Phrase -> errno. Multi-word substrings, matched case-insensitively. The table
 * is deliberately conservative: a phrase is here only when it maps to one errno
 * unambiguously. Order matters only for readability; lookups scan the whole
 * table.
 */
static const struct {
	const char *phrase;
	int code;
} phrase_map[] = {
	{"no such file", ENOENT},
	{"not found", ENOENT},
	{"does not exist", ENOENT},
	{"permission denied", EACCES},
	{"access denied", EACCES},
	{"operation not permitted", EPERM},
	{"already exists", EEXIST},
	{"file exists", EEXIST},
	{"not a directory", ENOTDIR},
	{"is a directory", EISDIR},
	{"no space left", ENOSPC},
	{"disk full", ENOSPC},
	{"read-only file system", EROFS},
	{"read-only", EROFS},
	{"too many open files", EMFILE},
	{"file name too long", ENAMETOOLONG},
	{"name too long", ENAMETOOLONG},
	{"directory not empty", ENOTEMPTY},
	{NULL, 0},
};

static const struct {
	int code;
	const char *name;
} name_map[] = {
	{ENOENT, "ENOENT"},
	{EACCES, "EACCES"},
	{EPERM, "EPERM"},
	{EEXIST, "EEXIST"},
	{ENOTDIR, "ENOTDIR"},
	{EISDIR, "EISDIR"},
	{ENOSPC, "ENOSPC"},
	{EROFS, "EROFS"},
	{EMFILE, "EMFILE"},
	{ENFILE, "ENFILE"},
	{ENAMETOOLONG, "ENAMETOOLONG"},
	{ENOTEMPTY, "ENOTEMPTY"},
	{EXDEV, "EXDEV"},
	{ELOOP, "ELOOP"},
	{0, NULL},
};

int errno_family(int code)
{
	/* EACCES and EPERM are both "you may not"; a program that says one while
	 * the kernel returned the other is not mistranslating. */
	if (code == EPERM)
		return EACCES;
	return code;
}

const char *errno_name(int code)
{
	for (int i = 0; name_map[i].name; i++)
		if (name_map[i].code == code)
			return name_map[i].name;
	return NULL;
}

int errno_phrases_in_line(const char *line, int *out, int max)
{
	int n = 0;
	for (int i = 0; phrase_map[i].phrase && n < max; i++) {
		if (!strcasestr(line, phrase_map[i].phrase))
			continue;
		int fam = errno_family(phrase_map[i].code);
		int seen = 0;
		for (int j = 0; j < n; j++)
			if (errno_family(out[j]) == fam) {
				seen = 1;
				break;
			}
		if (!seen)
			out[n++] = phrase_map[i].code;
	}
	return n;
}
