#include "report.h"

#include <stdio.h>
#include <string.h>

/* ---- human-readable report ---- */

static const char *finding_label(enum finding_type t)
{
	switch (t) {
	case F_FAITHLESS_CLAIM:
		return "faithless claim";
	case F_UNDECLARED_WRITE:
		return "undeclared write";
	case F_DISCOURSE_MISMATCH:
		return "discourse mismatch";
	}
	return "?";
}

void report_human(FILE *out, const struct audit *au, const struct vec *findings)
{
	if (findings->len == 0) {
		fprintf(out, "verba: no contradictions found.\n");
		return;
	}

	fprintf(out, "verba: %zu contradiction%s between words and syscalls:\n\n",
	        findings->len, findings->len == 1 ? "" : "s");

	for (size_t i = 0; i < findings->len; i++) {
		const struct finding *f = findings->data[i];
		fprintf(out, "  [%s]", finding_label(f->type));
		if (f->path)
			fprintf(out, " %s", f->path);
		fprintf(out, "\n");
		if (f->detail)
			fprintf(out, "      %s\n", f->detail);
		if (f->evidence_word)
			fprintf(out, "      words: \"%s\"\n", f->evidence_word);
		if (f->evidence_deed)
			fprintf(out, "      deed:  %s\n", f->evidence_deed);
		fprintf(out, "\n");
	}

	if (au->exited)
		fprintf(out, "command exited with status %d.\n", au->exit_code);
	else if (au->signaled)
		fprintf(out, "command killed by signal %d.\n", au->term_signal);
}

/* ---- JSON report ---- */

static void json_str(FILE *out, const char *s)
{
	if (!s) {
		fputs("null", out);
		return;
	}
	fputc('"', out);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':
			fputs("\\\"", out);
			break;
		case '\\':
			fputs("\\\\", out);
			break;
		case '\n':
			fputs("\\n", out);
			break;
		case '\r':
			fputs("\\r", out);
			break;
		case '\t':
			fputs("\\t", out);
			break;
		case '\b':
			fputs("\\b", out);
			break;
		case '\f':
			fputs("\\f", out);
			break;
		default:
			if (*p < 0x20)
				fprintf(out, "\\u%04x", *p);
			else
				fputc(*p, out);
		}
	}
	fputc('"', out);
}

void report_json(FILE *out, const struct audit *au, const struct vec *findings)
{
	fputs("{\n", out);
	fputs("  \"schema\": 1,\n", out);

	fputs("  \"command\": [", out);
	for (int i = 0; au->argv && au->argv[i]; i++) {
		if (i)
			fputs(", ", out);
		json_str(out, au->argv[i]);
	}
	fputs("],\n", out);

	fputs("  \"exit\": {", out);
	fprintf(out, "\"exited\": %s, ", au->exited ? "true" : "false");
	fprintf(out, "\"code\": %d, ", au->exit_code);
	fprintf(out, "\"signal\": %d, ", au->signaled ? au->term_signal : 0);
	fprintf(out, "\"exec_failed\": %s},\n",
	        au->exec_failed ? "true" : "false");

	/* findings */
	fputs("  \"findings\": [", out);
	for (size_t i = 0; i < findings->len; i++) {
		const struct finding *f = findings->data[i];
		fputs(i ? ",\n" : "\n", out);
		fputs("    {\"type\": ", out);
		json_str(out, finding_type_name(f->type));
		fputs(", \"path\": ", out);
		json_str(out, f->path);
		fputs(", \"detail\": ", out);
		json_str(out, f->detail);
		fputs(", \"words\": ", out);
		json_str(out, f->evidence_word);
		fputs(", \"deed\": ", out);
		json_str(out, f->evidence_deed);
		fputc('}', out);
	}
	fputs(findings->len ? "\n  ],\n" : "],\n", out);

	/* claims (captured output) */
	fputs("  \"claims\": [", out);
	for (size_t i = 0; i < au->lines.len; i++) {
		const struct out_line *l = au->lines.data[i];
		fputs(i ? ",\n" : "\n", out);
		fprintf(out, "    {\"order\": %lu, \"stream\": %s, \"text\": ",
		        l->order, l->stream == 2 ? "\"stderr\"" : "\"stdout\"");
		json_str(out, l->text);
		fputc('}', out);
	}
	fputs(au->lines.len ? "\n  ],\n" : "],\n", out);

	/* trace (file syscalls) */
	fputs("  \"trace\": [", out);
	for (size_t i = 0; i < au->events.len; i++) {
		const struct file_event *e = au->events.data[i];
		fputs(i ? ",\n" : "\n", out);
		fprintf(out, "    {\"pid\": %d, \"syscall\": ", (int)e->pid);
		json_str(out, e->sysname);
		fputs(", \"action\": ", out);
		json_str(out, file_action_name(e->action));
		fputs(", \"path\": ", out);
		json_str(out, e->path);
		fputs(", \"path2\": ", out);
		json_str(out, e->path2);
		fprintf(out, ", \"result\": %ld, \"errno\": %d}", e->result,
		        e->err);
	}
	fputs(au->events.len ? "\n  ]\n" : "]\n", out);

	fputs("}\n", out);
}
