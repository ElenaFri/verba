#include "trace.h"
#include "arch.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __WALL
#define __WALL 0x40000000
#endif

/* PTRACE_GET_SYSCALL_INFO (Linux 5.3+) tells us unambiguously whether a
 * syscall-stop is an entry or an exit, which a naive enter/exit toggle gets
 * wrong around execve and freshly-cloned children. Defined defensively for
 * toolchains whose headers predate it; we read only the leading `op` byte. */
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
#ifndef PTRACE_SYSCALL_INFO_ENTRY
#define PTRACE_SYSCALL_INFO_ENTRY 1
#endif
#ifndef PTRACE_SYSCALL_INFO_EXIT
#define PTRACE_SYSCALL_INFO_EXIT 2
#endif

#define STR_MAX 4096 /* cap on a single path read from the tracee */

/* ---- per-tracee and per-fd bookkeeping ---- */

struct tracee {
	pid_t pid;
	int entering; /* next syscall-stop is an enter */
	int booted;   /* consumed the initial SIGSTOP of a new child */
	long nr;
	unsigned long args[6];
	char *patha; /* primary path arg, read at enter */
	char *pathb; /* secondary path arg (rename/link) */
	int existed; /* for O_CREAT: did the target exist beforehand */
};

struct openfd {
	pid_t pid;
	int fd;
	char *path;
	int written;
};

struct linebuf {
	char *buf;
	size_t len;
	size_t cap;
};

struct tctx {
	struct audit *au;
	pid_t child;
	unsigned long order;
	struct vec tracees; /* struct tracee*  */
	struct vec openfds; /* struct openfd*  */
	struct linebuf lb_out;
	struct linebuf lb_err;
};

/* ---- tracee map ---- */

static struct tracee *find_tracee(struct tctx *c, pid_t pid)
{
	for (size_t i = 0; i < c->tracees.len; i++) {
		struct tracee *t = c->tracees.data[i];
		if (t->pid == pid)
			return t;
	}
	return NULL;
}

static struct tracee *add_tracee(struct tctx *c, pid_t pid)
{
	struct tracee *t = xmalloc(sizeof *t);
	memset(t, 0, sizeof *t);
	t->pid = pid;
	t->entering = 1;
	vec_push(&c->tracees, t);
	return t;
}

static void remove_tracee(struct tctx *c, pid_t pid)
{
	for (size_t i = 0; i < c->tracees.len; i++) {
		struct tracee *t = c->tracees.data[i];
		if (t->pid != pid)
			continue;
		free(t->patha);
		free(t->pathb);
		free(t);
		c->tracees.data[i] = c->tracees.data[--c->tracees.len];
		return;
	}
}

/* ---- open-fd table (for write/ftruncate tracking) ---- */

static struct openfd *of_find(struct tctx *c, pid_t pid, int fd)
{
	for (size_t i = 0; i < c->openfds.len; i++) {
		struct openfd *o = c->openfds.data[i];
		if (o->pid == pid && o->fd == fd)
			return o;
	}
	return NULL;
}

static void of_add(struct tctx *c, pid_t pid, int fd, const char *path)
{
	struct openfd *o = of_find(c, pid, fd);
	if (o) { /* fd reused without an observed close */
		free(o->path);
		o->path = xstrdup(path);
		o->written = 0;
		return;
	}
	o = xmalloc(sizeof *o);
	o->pid = pid;
	o->fd = fd;
	o->path = xstrdup(path);
	o->written = 0;
	vec_push(&c->openfds, o);
}

static void of_remove(struct tctx *c, pid_t pid, int fd)
{
	for (size_t i = 0; i < c->openfds.len; i++) {
		struct openfd *o = c->openfds.data[i];
		if (o->pid == pid && o->fd == fd) {
			free(o->path);
			free(o);
			c->openfds.data[i] = c->openfds.data[--c->openfds.len];
			return;
		}
	}
}

/* ---- reading tracee memory and resolving paths via /proc ---- */

static char *read_string(pid_t pid, unsigned long addr)
{
	if (!addr)
		return NULL;

	size_t cap = 256, len = 0;
	char *buf = xmalloc(cap);

	for (;;) {
		errno = 0;
		long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + len), 0);
		if (word == -1 && errno)
			break;
		char *bytes = (char *)&word;
		for (size_t i = 0; i < sizeof(long); i++) {
			if (len + 1 >= cap) {
				cap *= 2;
				buf = xrealloc(buf, cap);
			}
			buf[len] = bytes[i];
			if (bytes[i] == '\0')
				return buf;
			len++;
			if (len >= STR_MAX) {
				buf[len] = '\0';
				return buf;
			}
		}
	}
	buf[len] = '\0';
	return buf;
}

static char *base_dir(pid_t pid, int dirfd)
{
	char link[64];
	if (dirfd == AT_FDCWD)
		snprintf(link, sizeof link, "/proc/%d/cwd", pid);
	else
		snprintf(link, sizeof link, "/proc/%d/fd/%d", pid, dirfd);
	return xreadlink(link);
}

static char *resolve_canon(pid_t pid, int dirfd, const char *raw)
{
	if (!raw)
		return NULL;

	char *abs;
	if (raw[0] == '/') {
		abs = xstrdup(raw);
	} else {
		char *base = base_dir(pid, dirfd);
		abs = path_join(base ? base : ".", raw);
		free(base);
	}
	char *canon = path_canon(abs);
	free(abs);
	return canon;
}

/* Canonical path of an open fd via /proc/<pid>/fd/<fd>. Non-file descriptors
 * (pipes, sockets) come back as e.g. "pipe:[12345]" and are returned as-is. */
static char *proc_fd_canon(pid_t pid, int fd)
{
	char link[64];
	snprintf(link, sizeof link, "/proc/%d/fd/%d", pid, fd);
	char *target = xreadlink(link);
	if (!target)
		return NULL;
	if (target[0] == '/') {
		char *canon = path_canon(target);
		free(target);
		return canon;
	}
	return target;
}

static int path_exists_at(pid_t pid, int dirfd, const char *raw)
{
	if (!raw || !raw[0])
		return 0;
	char *abs;
	if (raw[0] == '/') {
		abs = xstrdup(raw);
	} else {
		char *base = base_dir(pid, dirfd);
		abs = path_join(base ? base : ".", raw);
		free(base);
	}
	struct stat st;
	int r = (lstat(abs, &st) == 0);
	free(abs);
	return r;
}

/* ---- event recording (takes ownership of path/path2) ---- */

static void push_event(struct tctx *c, pid_t pid, enum file_action action,
                       const char *sysname, char *path, char *path2, long ret,
                       int err)
{
	if (!path) {
		free(path2);
		return;
	}
	struct file_event *e = xmalloc(sizeof *e);
	e->pid = pid;
	e->action = action;
	e->path = path;
	e->path2 = path2;
	e->result = ret;
	e->err = err;
	e->sysname = sysname;
	vec_push(&c->au->events, e);
}

/* ---- output capture ---- */

static void emit_line(struct tctx *c, int stream, const char *start, size_t len)
{
	while (len > 0 && start[len - 1] == '\r')
		len--;
	struct out_line *l = xmalloc(sizeof *l);
	l->order = c->order++;
	l->stream = stream;
	l->text = xstrndup(start, len);
	vec_push(&c->au->lines, l);
}

static void lb_feed(struct tctx *c, struct linebuf *lb, int stream,
                    const char *data, size_t n)
{
	if (lb->len + n + 1 > lb->cap) {
		while (lb->cap < lb->len + n + 1)
			lb->cap = lb->cap ? lb->cap * 2 : 256;
		lb->buf = xrealloc(lb->buf, lb->cap);
	}
	memcpy(lb->buf + lb->len, data, n);
	lb->len += n;

	size_t start = 0;
	for (size_t i = 0; i < lb->len; i++) {
		if (lb->buf[i] == '\n') {
			emit_line(c, stream, lb->buf + start, i - start);
			start = i + 1;
		}
	}
	if (start > 0) {
		memmove(lb->buf, lb->buf + start, lb->len - start);
		lb->len -= start;
	}
}

static void lb_flush(struct tctx *c, struct linebuf *lb, int stream)
{
	if (lb->len > 0)
		emit_line(c, stream, lb->buf, lb->len);
	lb->len = 0;
}

/* ---- syscall handling ---- */

static void handle_enter(struct tctx *c, struct tracee *t, const verba_regs *r)
{
	pid_t pid = t->pid;
	(void)c;
	t->entering = 0;
	t->nr = arch_syscall_nr(r);
	for (int i = 0; i < 6; i++)
		t->args[i] = arch_syscall_arg(r, i);
	free(t->patha);
	free(t->pathb);
	t->patha = t->pathb = NULL;
	t->existed = 0;

	switch (t->nr) {
#ifdef SYS_open
	case SYS_open:
		t->patha = read_string(pid, t->args[0]);
		if (t->args[1] & O_CREAT)
			t->existed = path_exists_at(pid, AT_FDCWD, t->patha);
		break;
#endif
#ifdef SYS_openat
	case SYS_openat:
		t->patha = read_string(pid, t->args[1]);
		if (t->args[2] & O_CREAT)
			t->existed = path_exists_at(pid, (int)t->args[0], t->patha);
		break;
#endif
#ifdef SYS_creat
	case SYS_creat:
		t->patha = read_string(pid, t->args[0]);
		t->existed = path_exists_at(pid, AT_FDCWD, t->patha);
		break;
#endif
#ifdef SYS_unlink
	case SYS_unlink:
		t->patha = read_string(pid, t->args[0]);
		break;
#endif
#ifdef SYS_unlinkat
	case SYS_unlinkat:
		t->patha = read_string(pid, t->args[1]);
		break;
#endif
#ifdef SYS_rename
	case SYS_rename:
		t->patha = read_string(pid, t->args[0]);
		t->pathb = read_string(pid, t->args[1]);
		break;
#endif
#ifdef SYS_renameat
	case SYS_renameat:
		t->patha = read_string(pid, t->args[1]);
		t->pathb = read_string(pid, t->args[3]);
		break;
#endif
#ifdef SYS_renameat2
	case SYS_renameat2:
		t->patha = read_string(pid, t->args[1]);
		t->pathb = read_string(pid, t->args[3]);
		break;
#endif
#ifdef SYS_link
	case SYS_link:
		t->patha = read_string(pid, t->args[0]);
		t->pathb = read_string(pid, t->args[1]);
		break;
#endif
#ifdef SYS_linkat
	case SYS_linkat:
		t->patha = read_string(pid, t->args[1]);
		t->pathb = read_string(pid, t->args[3]);
		break;
#endif
#ifdef SYS_symlink
	case SYS_symlink:
		t->patha = read_string(pid, t->args[1]); /* link path (created) */
		break;
#endif
#ifdef SYS_symlinkat
	case SYS_symlinkat:
		t->patha = read_string(pid, t->args[2]);
		break;
#endif
#ifdef SYS_mkdir
	case SYS_mkdir:
		t->patha = read_string(pid, t->args[0]);
		break;
#endif
#ifdef SYS_mkdirat
	case SYS_mkdirat:
		t->patha = read_string(pid, t->args[1]);
		break;
#endif
	default:
		break;
	}
}

static void do_open_exit(struct tctx *c, struct tracee *t, long ret, int err,
                         int flags, int dirfd, const char *sysname)
{
	int write_intent = ((flags & O_ACCMODE) != O_RDONLY);

	if (err == 0 && ret >= 0) {
		char *canonp = proc_fd_canon(t->pid, (int)ret);
		if (!canonp)
			canonp = resolve_canon(t->pid, dirfd, t->patha);
		if (!canonp)
			return;

		if (write_intent)
			push_event(c, t->pid, ACT_OPEN_WRITE, sysname,
			           xstrdup(canonp), NULL, ret, 0);
		if ((flags & O_CREAT) && !t->existed)
			push_event(c, t->pid, ACT_CREATE, sysname,
			           xstrdup(canonp), NULL, ret, 0);
		else if ((flags & O_TRUNC) && write_intent)
			push_event(c, t->pid, ACT_TRUNCATE, sysname,
			           xstrdup(canonp), NULL, ret, 0);
		if (write_intent)
			of_add(c, t->pid, (int)ret, canonp);
		free(canonp);
	} else if (err && write_intent) {
		push_event(c, t->pid, ACT_OPEN_WRITE, sysname,
		           resolve_canon(t->pid, dirfd, t->patha), NULL, ret, err);
	}
}

static void handle_exit(struct tctx *c, struct tracee *t, const verba_regs *r)
{
	pid_t pid = t->pid;
	t->entering = 1;
	long ret = arch_syscall_ret(r);
	int err = (ret < 0 && ret >= -4095) ? (int)(-ret) : 0;

	switch (t->nr) {
#ifdef SYS_open
	case SYS_open:
		do_open_exit(c, t, ret, err, (int)t->args[1], AT_FDCWD, "open");
		break;
#endif
#ifdef SYS_openat
	case SYS_openat:
		do_open_exit(c, t, ret, err, (int)t->args[2], (int)t->args[0],
		             "openat");
		break;
#endif
#ifdef SYS_creat
	case SYS_creat:
		do_open_exit(c, t, ret, err, O_WRONLY | O_CREAT | O_TRUNC,
		             AT_FDCWD, "creat");
		break;
#endif
#ifdef SYS_write
	case SYS_write:
		if (err == 0 && ret > 0) {
			struct openfd *o = of_find(c, pid, (int)t->args[0]);
			if (o && !o->written) {
				push_event(c, pid, ACT_WRITE, "write",
				           xstrdup(o->path), NULL, ret, 0);
				o->written = 1;
			}
		}
		break;
#endif
#ifdef SYS_ftruncate
	case SYS_ftruncate:
		if (err == 0) {
			struct openfd *o = of_find(c, pid, (int)t->args[0]);
			if (o)
				push_event(c, pid, ACT_TRUNCATE, "ftruncate",
				           xstrdup(o->path), NULL, ret, 0);
		}
		break;
#endif
#ifdef SYS_close
	case SYS_close:
		if (err == 0)
			of_remove(c, pid, (int)t->args[0]);
		break;
#endif
#ifdef SYS_unlink
	case SYS_unlink:
		if (err == 0)
			push_event(c, pid, ACT_UNLINK, "unlink",
			           resolve_canon(pid, AT_FDCWD, t->patha), NULL,
			           ret, 0);
		break;
#endif
#ifdef SYS_unlinkat
	case SYS_unlinkat:
		if (err == 0)
			push_event(c, pid, ACT_UNLINK, "unlinkat",
			           resolve_canon(pid, (int)t->args[0], t->patha),
			           NULL, ret, 0);
		break;
#endif
#ifdef SYS_mkdir
	case SYS_mkdir:
		if (err == 0)
			push_event(c, pid, ACT_CREATE, "mkdir",
			           resolve_canon(pid, AT_FDCWD, t->patha), NULL,
			           ret, 0);
		break;
#endif
#ifdef SYS_mkdirat
	case SYS_mkdirat:
		if (err == 0)
			push_event(c, pid, ACT_CREATE, "mkdirat",
			           resolve_canon(pid, (int)t->args[0], t->patha),
			           NULL, ret, 0);
		break;
#endif
#ifdef SYS_rename
	case SYS_rename:
		if (err == 0)
			push_event(c, pid, ACT_RENAME, "rename",
			           resolve_canon(pid, AT_FDCWD, t->pathb),
			           resolve_canon(pid, AT_FDCWD, t->patha), ret, 0);
		break;
#endif
#ifdef SYS_renameat
	case SYS_renameat:
		if (err == 0)
			push_event(c, pid, ACT_RENAME, "renameat",
			           resolve_canon(pid, (int)t->args[2], t->pathb),
			           resolve_canon(pid, (int)t->args[0], t->patha),
			           ret, 0);
		break;
#endif
#ifdef SYS_renameat2
	case SYS_renameat2:
		if (err == 0)
			push_event(c, pid, ACT_RENAME, "renameat2",
			           resolve_canon(pid, (int)t->args[2], t->pathb),
			           resolve_canon(pid, (int)t->args[0], t->patha),
			           ret, 0);
		break;
#endif
#ifdef SYS_link
	case SYS_link:
		if (err == 0)
			push_event(c, pid, ACT_CREATE, "link",
			           resolve_canon(pid, AT_FDCWD, t->pathb), NULL,
			           ret, 0);
		break;
#endif
#ifdef SYS_linkat
	case SYS_linkat:
		if (err == 0)
			push_event(c, pid, ACT_CREATE, "linkat",
			           resolve_canon(pid, (int)t->args[2], t->pathb),
			           NULL, ret, 0);
		break;
#endif
#ifdef SYS_symlink
	case SYS_symlink:
		if (err == 0)
			push_event(c, pid, ACT_CREATE, "symlink",
			           resolve_canon(pid, AT_FDCWD, t->patha), NULL,
			           ret, 0);
		break;
#endif
#ifdef SYS_symlinkat
	case SYS_symlinkat:
		if (err == 0)
			push_event(c, pid, ACT_CREATE, "symlinkat",
			           resolve_canon(pid, (int)t->args[1], t->patha),
			           NULL, ret, 0);
		break;
#endif
	default:
		break;
	}

	free(t->patha);
	free(t->pathb);
	t->patha = t->pathb = NULL;
}

/* ---- the ptrace loop ---- */

static void resume(pid_t pid, int sig)
{
	ptrace(PTRACE_SYSCALL, pid, 0, (void *)(long)sig);
}

/* 1 = syscall entry, 2 = syscall exit, 0 = unknown (kernel too old). */
static int syscall_dir(pid_t pid)
{
	/* Zero-initialised: PTRACE_GET_SYSCALL_INFO is not understood by every
	 * valgrind build, so its write to the buffer may go untracked. Priming
	 * the leading byte keeps us valgrind-clean without changing behaviour. */
	unsigned char info[128] = {0};
	long n = ptrace(PTRACE_GET_SYSCALL_INFO, pid,
	                (void *)(uintptr_t)sizeof(info), info);
	if (n <= 0)
		return 0;
	return info[0];
}

static void handle_wait(struct tctx *c, pid_t pid, int status)
{
	struct tracee *t = find_tracee(c, pid);
	int is_new = 0;
	if (!t) {
		t = add_tracee(c, pid);
		is_new = 1;
	}

	if (WIFEXITED(status) || WIFSIGNALED(status)) {
		if (pid == c->child) {
			if (WIFEXITED(status)) {
				c->au->exited = 1;
				c->au->exit_code = WEXITSTATUS(status);
			} else {
				c->au->signaled = 1;
				c->au->term_signal = WTERMSIG(status);
			}
		}
		remove_tracee(c, pid);
		return;
	}

	if (!WIFSTOPPED(status))
		return;

	int sig = WSTOPSIG(status);
	unsigned event = ((unsigned)status >> 16) & 0xff;

	if (sig == (SIGTRAP | 0x80)) {
		verba_regs regs;
		if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == 0) {
			int dir = syscall_dir(pid);
			if (dir == PTRACE_SYSCALL_INFO_ENTRY)
				handle_enter(c, t, &regs);
			else if (dir == PTRACE_SYSCALL_INFO_EXIT)
				handle_exit(c, t, &regs);
			else if (t->entering) /* fallback: kernel < 5.3 */
				handle_enter(c, t, &regs);
			else
				handle_exit(c, t, &regs);
		}
		resume(pid, 0);
	} else if (sig == SIGTRAP && event != 0) {
		if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK ||
		    event == PTRACE_EVENT_CLONE) {
			unsigned long newpid = 0;
			ptrace(PTRACE_GETEVENTMSG, pid, 0, &newpid);
			if (newpid && !find_tracee(c, (pid_t)newpid))
				add_tracee(c, (pid_t)newpid);
		} else if (event == PTRACE_EVENT_EXEC) {
			t->entering = 1;
			free(t->patha);
			free(t->pathb);
			t->patha = t->pathb = NULL;
		}
		resume(pid, 0);
	} else {
		int deliver = sig;
		if (sig == SIGSTOP && !t->booted) {
			t->booted = 1;
			deliver = 0;
		} else if (sig == SIGTRAP) {
			deliver = 0;
		}
		(void)is_new;
		resume(pid, deliver);
	}
}

static int drain_pipe(struct tctx *c, int fd, int stream, struct linebuf *lb)
{
	char buf[4096];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof buf);
		if (n > 0) {
			lb_feed(c, lb, stream, buf, (size_t)n);
			continue;
		}
		if (n == 0)
			return 0; /* EOF */
		if (errno == EINTR)
			continue;
		return 1; /* EAGAIN: still open */
	}
}

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int trace_run(char **argv, struct audit *au)
{
	int outp[2], errp[2], execp[2];
	if (pipe(outp) || pipe(errp) || pipe2(execp, O_CLOEXEC))
		return -1;

	sigset_t mask, old;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, &old);
	int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd < 0) {
		sigprocmask(SIG_SETMASK, &old, NULL);
		return -1;
	}

	pid_t child = fork();
	if (child < 0) {
		close(sfd);
		sigprocmask(SIG_SETMASK, &old, NULL);
		return -1;
	}

	if (child == 0) {
		sigprocmask(SIG_SETMASK, &old, NULL);
		dup2(outp[1], STDOUT_FILENO);
		dup2(errp[1], STDERR_FILENO);
		close(outp[0]);
		close(outp[1]);
		close(errp[0]);
		close(errp[1]);
		close(sfd);
		close(execp[0]);
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		execvp(argv[0], argv);
		int e = errno;
		ssize_t w = write(execp[1], &e, sizeof e);
		(void)w;
		_exit(127);
	}

	/* parent */
	close(outp[1]);
	close(errp[1]);
	close(execp[1]);

	au->argv = argv;

	/* Synchronise on exec: the CLOEXEC pipe reaches EOF on success, or
	 * carries errno on failure. */
	int exec_errno = 0;
	ssize_t r;
	do {
		r = read(execp[0], &exec_errno, sizeof exec_errno);
	} while (r < 0 && errno == EINTR);
	close(execp[0]);

	if (r == (ssize_t)sizeof exec_errno) {
		int st;
		waitpid(child, &st, 0);
		au->exec_failed = 1;
		au->exec_errno = exec_errno;
		close(outp[0]);
		close(errp[0]);
		close(sfd);
		sigprocmask(SIG_SETMASK, &old, NULL);
		return 0;
	}

	struct tctx c;
	memset(&c, 0, sizeof c);
	c.au = au;
	c.child = child;

	/* Initial exec-trap stop: set options, then start syscall tracing. */
	int st;
	if (waitpid(child, &st, 0) < 0) {
		close(outp[0]);
		close(errp[0]);
		close(sfd);
		sigprocmask(SIG_SETMASK, &old, NULL);
		return -1;
	}
	long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK |
	            PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
	            PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
	ptrace(PTRACE_SETOPTIONS, child, 0, (void *)opts);
	struct tracee *ct = add_tracee(&c, child);
	ct->booted = 1;
	resume(child, 0);

	int outfd = outp[0], errfd = errp[0];
	set_nonblock(outfd);
	set_nonblock(errfd);
	int out_open = 1, err_open = 1;

	while (c.tracees.len > 0 || out_open || err_open) {
		struct pollfd pfd[3];
		int n = 0, isig = -1, iout = -1, ierr = -1;

		if (c.tracees.len > 0) {
			pfd[n].fd = sfd;
			pfd[n].events = POLLIN;
			isig = n++;
		}
		if (out_open) {
			pfd[n].fd = outfd;
			pfd[n].events = POLLIN;
			iout = n++;
		}
		if (err_open) {
			pfd[n].fd = errfd;
			pfd[n].events = POLLIN;
			ierr = n++;
		}
		if (n == 0)
			break;

		if (poll(pfd, (nfds_t)n, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (isig >= 0 && (pfd[isig].revents & POLLIN)) {
			struct signalfd_siginfo si;
			while (read(sfd, &si, sizeof si) > 0)
				;
			pid_t w;
			int ws;
			while ((w = waitpid(-1, &ws, WNOHANG | __WALL)) > 0)
				handle_wait(&c, w, ws);
		}
		if (iout >= 0 && (pfd[iout].revents & (POLLIN | POLLHUP))) {
			if (!drain_pipe(&c, outfd, 1, &c.lb_out))
				out_open = 0;
		}
		if (ierr >= 0 && (pfd[ierr].revents & (POLLIN | POLLHUP))) {
			if (!drain_pipe(&c, errfd, 2, &c.lb_err))
				err_open = 0;
		}
	}

	lb_flush(&c, &c.lb_out, 1);
	lb_flush(&c, &c.lb_err, 2);

	/* cleanup */
	for (size_t i = 0; i < c.tracees.len; i++) {
		struct tracee *t = c.tracees.data[i];
		free(t->patha);
		free(t->pathb);
		free(t);
	}
	vec_free(&c.tracees, NULL);
	for (size_t i = 0; i < c.openfds.len; i++) {
		struct openfd *o = c.openfds.data[i];
		free(o->path);
		free(o);
	}
	vec_free(&c.openfds, NULL);
	free(c.lb_out.buf);
	free(c.lb_err.buf);
	close(outfd);
	close(errfd);
	close(sfd);
	sigprocmask(SIG_SETMASK, &old, NULL);
	return 0;
}
