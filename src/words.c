#include "words.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Phase-1 pattern lists: minimal, explicit, English-only (see TODO.md §1.4 open
 * decision). Multi-word phrases are matched as plain substrings; single words
 * require alphabetic word boundaries so "ok" does not fire inside "broken".
 */
static const char *const write_verbs[] = {
	"saved", "saving", "wrote", "writing", "written",
	"created", "creating", "generated", "updated", NULL,
};

static const char *const success_words[] = {
	"success", "successful", "successfully", "done",
	"complete", "completed", "finished", "ok", NULL,
};

static const char *const error_words[] = {
	"error",   "failed",    "failure",   "cannot",
	"unable",  "not found", "no such",   "permission denied",
	"denied",  "fatal",     "aborted",   NULL,
};

static int is_word_char(int c)
{
	return isalnum((unsigned char)c) || c == '_';
}

/* Case-insensitive search; single-word needles must sit on word boundaries. */
static int has_pattern(const char *line, const char *needle)
{
	int phrase = (strchr(needle, ' ') != NULL);
	const char *p = line;
	size_t nl = strlen(needle);

	while ((p = strcasestr(p, needle)) != NULL) {
		if (phrase)
			return 1;
		int left_ok = (p == line) || !is_word_char((unsigned char)p[-1]);
		int right_ok = !is_word_char((unsigned char)p[nl]);
		if (left_ok && right_ok)
			return 1;
		p += 1;
	}
	return 0;
}

static int any_pattern(const char *line, const char *const list[])
{
	for (int i = 0; list[i]; i++)
		if (has_pattern(line, list[i]))
			return 1;
	return 0;
}

int words_is_write_claim(const char *line)
{
	return any_pattern(line, write_verbs);
}

int words_is_success(const char *line)
{
	return any_pattern(line, success_words);
}

int words_is_error(const char *line)
{
	return any_pattern(line, error_words);
}

/* Trim characters that commonly bracket a path in prose. */
static char *trim_token(const char *tok)
{
	const char *lead = "'\"`([{<";
	const char *trail = "'\"`)]}>.,:;!?";

	while (*tok && strchr(lead, *tok))
		tok++;
	size_t len = strlen(tok);
	while (len > 0 && strchr(trail, tok[len - 1]))
		len--;
	return xstrndup(tok, len);
}

static int looks_like_path(const char *tok)
{
	if (strstr(tok, "://")) /* skip URLs */
		return 0;
	if (tok[0] == '/')
		return 1;
	if (tok[0] == '~' && (tok[1] == '/' || tok[1] == '\0'))
		return 1;
	if (strncmp(tok, "./", 2) == 0 || strncmp(tok, "../", 3) == 0)
		return 1;
	if (strncmp(tok, "$HOME", 5) == 0)
		return 1;
	/* A bare relative word only counts when it actually contains a slash. */
	if (strchr(tok, '/'))
		return 1;
	return 0;
}

/* Expand ~ / $HOME and make absolute against cwd; returns owned absolute path. */
static char *to_absolute(const char *tok)
{
	const char *home = getenv("HOME");
	char *expanded;

	if (tok[0] == '~' && (tok[1] == '/' || tok[1] == '\0')) {
		expanded = path_join(home ? home : "", tok[1] ? tok + 2 : "");
	} else if (strncmp(tok, "$HOME", 5) == 0) {
		const char *rest = tok + 5;
		if (*rest == '/')
			rest++;
		expanded = path_join(home ? home : "", rest);
	} else {
		expanded = xstrdup(tok);
	}

	if (expanded[0] == '/')
		return expanded;

	char cwd[4096];
	char *abs;
	if (getcwd(cwd, sizeof cwd))
		abs = path_join(cwd, expanded);
	else
		abs = path_join("/", expanded);
	free(expanded);
	return abs;
}

void words_extract_paths(const char *line, struct vec *out)
{
	char *copy = xstrdup(line);
	char *save = NULL;

	for (char *tok = strtok_r(copy, " \t\r\n", &save); tok;
	     tok = strtok_r(NULL, " \t\r\n", &save)) {
		char *clean = trim_token(tok);
		if (clean[0] == '\0' || !looks_like_path(clean)) {
			free(clean);
			continue;
		}
		char *abs = to_absolute(clean);
		char *canon = path_canon(abs);
		free(abs);
		free(clean);
		vec_push(out, canon);
	}
	free(copy);
}
