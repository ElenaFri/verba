#ifndef VERBA_MODEL_H
#define VERBA_MODEL_H

#include <sys/types.h>

#include "vec.h"

/* ----- the words: a line of captured program output ----- */

struct out_line {
	unsigned long order; /* global interleave counter */
	int stream;          /* 0 = tty (combined), 1 = stdout, 2 = stderr */
	char *text;          /* line content, newline stripped */
	char *cwd;           /* dir to resolve this line's relative paths against,
	                      * captured when the line was emitted; NULL = use
	                      * verba's own cwd (pipe/pty mode has no per-line
	                      * attribution; only --follow knows the writer) */
};

/* ----- the deeds: one file-affecting syscall, as observed ----- */

enum file_action {
	ACT_OPEN_WRITE, /* opened with write intent (may or may not be written) */
	ACT_CREATE,     /* brought into existence (creat, O_CREAT new, mkdir, ...) */
	ACT_TRUNCATE,   /* O_TRUNC / ftruncate */
	ACT_WRITE,      /* write(2) to a write-opened file */
	ACT_UNLINK,     /* removed */
	ACT_RENAME,     /* renamed (path = new, path2 = old) */
};

struct file_event {
	pid_t pid;
	enum file_action action;
	char *path;  /* canonical absolute path, best effort */
	char *path2; /* rename source; else NULL */
	long result; /* raw syscall return */
	int err;     /* errno if the call failed, else 0 */
	const char *sysname;
};

/* ----- findings: contradictions between words and deeds ----- */

enum finding_type {
	F_FAITHLESS_CLAIM,
	F_UNDECLARED_WRITE,
	F_DISCOURSE_MISMATCH,
	F_MISTRANSLATION,
};

struct finding {
	enum finding_type type;
	int heuristic;       /* 1 = phase-2 heuristic, 0 = mechanical */
	char *path;          /* path involved, or NULL */
	char *evidence_word; /* the offending output line, or NULL */
	char *evidence_deed; /* the relevant syscall, or NULL */
	char *detail;        /* one-sentence explanation */
};

/* ----- the whole audit ----- */

struct audit {
	struct vec lines;  /* each item: one out_line */
	struct vec events; /* each item: one file_event */
	char **argv;       /* the traced command (not owned) */
	int exited;        /* child called exit() */
	int exit_code;     /* ...with this status */
	int signaled;      /* child was killed by a signal */
	int term_signal;   /* ...this one */
	int exec_failed;   /* the command could not be started */
	int exec_errno;
};

const char *file_action_name(enum file_action a);
const char *finding_type_name(enum finding_type t);

void out_line_free(void *p);
void file_event_free(void *p);
void finding_free(void *p);

/* Append a finding (copies every string). `heuristic` tags it phase-2. */
void findings_add(struct vec *findings, enum finding_type type, int heuristic,
                  const char *path, const char *word, const char *deed,
                  const char *detail);

/* Count findings whose heuristic flag matches `heuristic` (0 or 1). */
size_t findings_count(const struct vec *findings, int heuristic);

void audit_free(struct audit *au);
void findings_free(struct vec *findings);

#endif /* VERBA_MODEL_H */
