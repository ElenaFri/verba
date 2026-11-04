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
	struct vec tracees; /* each item: one tracee */
	struct vec openfds; /* each item: one openfd */
	struct linebuf lb_out;
	struct linebuf lb_err;
};

/* ---- tracee map ---- */

static struct tracee *find_tracee(const struct tctx *c, pid_t pid)
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

static struct openfd *of_find(const struct tctx *c, pid_t pid, int fd)
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
	/* LCOV_EXCL_START: fd reuse without an observed close is defensive. */
	if (o) {
		free(o->path);
		o->path = xstrdup(path);
		o->written = 0;
		return;
	}
	/* LCOV_EXCL_STOP */
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

	size_t cap = 256;
	size_t len = 0;
	char *buf = xmalloc(cap);

	for (;;) {
		errno = 0;
		long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + len), 0);
		if (word == -1 && errno)
			break;
		const char *bytes = (const char *)&word;
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
		return NULL; /* LCOV_EXCL_LINE: a live fd's /proc link always resolves */
	if (target[0] == '/') {
		char *canon = path_canon(target);
		free(target);
		return canon;
	}
	return target; /* LCOV_EXCL_LINE: write-opened fds are regular files */
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
                       const char *sysname, char *path, char *path2, long ret)
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
	e->err = (ret < 0 && ret >= -4095) ? (int)(-ret) : 0;
	e->sysname = sysname;
	vec_push(&c->au->events, e);
}

/* ---- output capture ---- */

static void emit_line(struct audit *au, unsigned long *order, int stream,
                      const char *start, size_t len)
{
	while (len > 0 && start[len - 1] == '\r')
		len--;
	struct out_line *l = xmalloc(sizeof *l);
	l->order = (*order)++;
	l->stream = stream;
	l->text = xstrndup(start, len);
	vec_push(&au->lines, l);
}

static void lb_feed(struct tctx *c, struct linebuf *lb, int stream,
                    const char *data, size_t n)
{
	size_t need = lb->len + n + 1;
	if (need > lb->cap) {
		lb->cap = need;
		lb->buf = xrealloc(lb->buf, lb->cap);
	}
	memcpy(lb->buf + lb->len, data, n);
	lb->len += n;

	size_t start = 0;
	for (size_t i = 0; i < lb->len; i++) {
		if (lb->buf[i] == '\n') {
			emit_line(c->au, &c->order, stream, lb->buf + start, i - start);
			start = i + 1;
		}
	}
	/* Shift any partial trailing line to the front. The copy runs forward and
	 * the destination index trails the source (start > 0), so it is a safe
	 * left shift; an element copy also sidesteps a bulk memmove whose bound the
	 * analyzer cannot track across the emit_line calls above. */
	if (start > 0) {
		size_t rem = lb->len - start;
		for (size_t i = 0; i < rem; i++)
			lb->buf[i] = lb->buf[start + i];
		lb->len = rem;
	}
}

static void lb_flush(struct tctx *c, struct linebuf *lb, int stream)
{
	if (lb->len > 0)
		emit_line(c->au, &c->order, stream, lb->buf, lb->len);
	lb->len = 0;
}

/* ---- syscall handling ---- */

static void handle_enter(const struct tctx *c, struct tracee *t,
                         const verba_regs *r)
{
	pid_t pid = t->pid;
	(void)c;
	t->entering = 0;
	t->nr = arch_syscall_nr(r);
	for (int i = 0; i < 6; i++)
		t->args[i] = arch_syscall_arg(r, i);
	free(t->patha);
	free(t->pathb);
	t->patha = NULL;
	t->pathb = NULL;
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
	/* Two-path syscalls, grouped by where their path arguments sit. */
#if defined(SYS_rename) || defined(SYS_link)
#ifdef SYS_rename
	case SYS_rename:
#endif
#ifdef SYS_link
	case SYS_link:
#endif
		t->patha = read_string(pid, t->args[0]);
		t->pathb = read_string(pid, t->args[1]);
		break;
#endif
#if defined(SYS_renameat) || defined(SYS_renameat2) || defined(SYS_linkat)
#ifdef SYS_renameat
	case SYS_renameat:
#endif
#ifdef SYS_renameat2
	case SYS_renameat2:
#endif
#ifdef SYS_linkat
	case SYS_linkat:
#endif
		t->patha = read_string(pid, t->args[1]);
		t->pathb = read_string(pid, t->args[3]);
		break;
#endif
	default:
		break;
	}
}

static void do_open_exit(struct tctx *c, const struct tracee *t, long ret,
                         int err, int flags, int dirfd, const char *sysname)
{
	int write_intent = ((flags & O_ACCMODE) != O_RDONLY);

	if (err == 0 && ret >= 0) {
		char *canonp = proc_fd_canon(t->pid, (int)ret);
		if (!canonp)
			canonp = resolve_canon(t->pid, dirfd, t->patha); /* LCOV_EXCL_LINE */
		if (!canonp)
			return; /* LCOV_EXCL_LINE */

		if (write_intent)
			push_event(c, t->pid, ACT_OPEN_WRITE, sysname,
			           xstrdup(canonp), NULL, ret);
		if ((flags & O_CREAT) && !t->existed)
			push_event(c, t->pid, ACT_CREATE, sysname,
			           xstrdup(canonp), NULL, ret);
		else if ((flags & O_TRUNC) && write_intent)
			push_event(c, t->pid, ACT_TRUNCATE, sysname,
			           xstrdup(canonp), NULL, ret);
		if (write_intent)
			of_add(c, t->pid, (int)ret, canonp);
		free(canonp);
	} else if (err && write_intent) {
		push_event(c, t->pid, ACT_OPEN_WRITE, sysname,
		           resolve_canon(t->pid, dirfd, t->patha), NULL, ret);
	}
}

/* write / ftruncate / close: fd-indexed, resolved through the open-fd table. */
static void do_fd_exit(struct tctx *c, const struct tracee *t, pid_t pid,
                       long ret, int err)
{
	switch (t->nr) {
#ifdef SYS_write
	case SYS_write:
		if (err == 0 && ret > 0) {
			struct openfd *o = of_find(c, pid, (int)t->args[0]);
			if (o && !o->written) {
				push_event(c, pid, ACT_WRITE, "write",
				           xstrdup(o->path), NULL, ret);
				o->written = 1;
			}
		}
		break;
#endif
#ifdef SYS_ftruncate
	case SYS_ftruncate:
		if (err == 0) {
			const struct openfd *o = of_find(c, pid, (int)t->args[0]);
			if (o)
				push_event(c, pid, ACT_TRUNCATE, "ftruncate",
				           xstrdup(o->path), NULL, ret);
		}
		break;
#endif
#ifdef SYS_close
	case SYS_close:
		if (err == 0)
			of_remove(c, pid, (int)t->args[0]);
		break;
#endif
	default:
		break;
	}
}

/* unlink / mkdir / rename / link / symlink (and *at forms): resolve the saved
 * enter-time path(s) and record the deed. None of these report on failure. */
static void do_path_create_exit(struct tctx *c, const struct tracee *t,
                                pid_t pid, long ret, int err)
{
	if (err != 0)
		return;

	switch (t->nr) {
#ifdef SYS_unlink
	case SYS_unlink:
		push_event(c, pid, ACT_UNLINK, "unlink",
		           resolve_canon(pid, AT_FDCWD, t->patha), NULL, ret);
		break;
#endif
#ifdef SYS_unlinkat
	case SYS_unlinkat:
		push_event(c, pid, ACT_UNLINK, "unlinkat",
		           resolve_canon(pid, (int)t->args[0], t->patha), NULL, ret);
		break;
#endif
#ifdef SYS_mkdir
	case SYS_mkdir:
		push_event(c, pid, ACT_CREATE, "mkdir",
		           resolve_canon(pid, AT_FDCWD, t->patha), NULL, ret);
		break;
#endif
#ifdef SYS_mkdirat
	case SYS_mkdirat:
		push_event(c, pid, ACT_CREATE, "mkdirat",
		           resolve_canon(pid, (int)t->args[0], t->patha), NULL, ret);
		break;
#endif
#ifdef SYS_rename
	case SYS_rename:
		push_event(c, pid, ACT_RENAME, "rename",
		           resolve_canon(pid, AT_FDCWD, t->pathb),
		           resolve_canon(pid, AT_FDCWD, t->patha), ret);
		break;
#endif
#ifdef SYS_renameat
	case SYS_renameat:
		push_event(c, pid, ACT_RENAME, "renameat",
		           resolve_canon(pid, (int)t->args[2], t->pathb),
		           resolve_canon(pid, (int)t->args[0], t->patha), ret);
		break;
#endif
#ifdef SYS_renameat2
	case SYS_renameat2:
		push_event(c, pid, ACT_RENAME, "renameat2",
		           resolve_canon(pid, (int)t->args[2], t->pathb),
		           resolve_canon(pid, (int)t->args[0], t->patha), ret);
		break;
#endif
#ifdef SYS_link
	case SYS_link:
		push_event(c, pid, ACT_CREATE, "link",
		           resolve_canon(pid, AT_FDCWD, t->pathb), NULL, ret);
		break;
#endif
#ifdef SYS_linkat
	case SYS_linkat:
		push_event(c, pid, ACT_CREATE, "linkat",
		           resolve_canon(pid, (int)t->args[2], t->pathb), NULL, ret);
		break;
#endif
#ifdef SYS_symlink
	case SYS_symlink:
		push_event(c, pid, ACT_CREATE, "symlink",
		           resolve_canon(pid, AT_FDCWD, t->patha), NULL, ret);
		break;
#endif
#ifdef SYS_symlinkat
	case SYS_symlinkat:
		push_event(c, pid, ACT_CREATE, "symlinkat",
		           resolve_canon(pid, (int)t->args[1], t->patha), NULL, ret);
		break;
#endif
	default:
		break;
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
	default:
		do_fd_exit(c, t, pid, ret, err);
		do_path_create_exit(c, t, pid, ret, err);
		break;
	}

	free(t->patha);
	free(t->pathb);
	t->patha = NULL;
	t->pathb = NULL;
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
		return 0; /* LCOV_EXCL_LINE: non-positive only on kernels < 5.3 */
	return info[0];
}

/* The child (or the whole group leader) is gone: record how, then forget it. */
static void handle_termination(struct tctx *c, pid_t pid, int status)
{
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
}

/* A syscall-stop: dispatch to enter/exit using the kernel's direction hint. */
static void handle_syscall_stop(struct tctx *c, struct tracee *t)
{
	verba_regs regs;
	if (ptrace(PTRACE_GETREGS, t->pid, 0, &regs) != 0)
		return; /* LCOV_EXCL_LINE: a GETREGS failure is defensive */

	int dir = syscall_dir(t->pid);
	if (dir == PTRACE_SYSCALL_INFO_ENTRY)
		handle_enter(c, t, &regs);
	else if (dir == PTRACE_SYSCALL_INFO_EXIT)
		handle_exit(c, t, &regs);
	/* LCOV_EXCL_START: enter/exit toggle fallback for kernels < 5.3. */
	else if (t->entering)
		handle_enter(c, t, &regs);
	else
		handle_exit(c, t, &regs);
	/* LCOV_EXCL_STOP */
}

/* A PTRACE_EVENT_* stop: pick up new children, reset state across execve. */
static void handle_ptrace_event(struct tctx *c, struct tracee *t, unsigned event)
{
	if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK ||
	    event == PTRACE_EVENT_CLONE) {
		unsigned long newpid = 0;
		ptrace(PTRACE_GETEVENTMSG, t->pid, 0, &newpid);
		if (newpid && !find_tracee(c, (pid_t)newpid))
			add_tracee(c, (pid_t)newpid);
	} else if (event == PTRACE_EVENT_EXEC) {
		t->entering = 1;
		free(t->patha);
		free(t->pathb);
		t->patha = NULL;
		t->pathb = NULL;
	}
}

/* Which signal to re-inject on a plain signal-delivery stop (0 = swallow). */
static int stop_signal_to_deliver(struct tracee *t, int sig)
{
	if (sig == SIGSTOP && !t->booted) {
		t->booted = 1;
		return 0;
	}
	if (sig == SIGTRAP)
		return 0; /* LCOV_EXCL_LINE: a stray plain SIGTRAP is defensive */
	return sig;
}

static void handle_wait(struct tctx *c, pid_t pid, int status)
{
	struct tracee *t = find_tracee(c, pid);
	if (!t)
		/* A freshly cloned child whose stop is reaped before its parent's
		 * PTRACE_EVENT_FORK: adopt it now (scheduling-order dependent). */
		t = add_tracee(c, pid);

	if (WIFEXITED(status) || WIFSIGNALED(status)) {
		handle_termination(c, pid, status);
		return;
	}
	if (!WIFSTOPPED(status))
		return; /* LCOV_EXCL_LINE: WIFCONTINUED is never requested */

	int sig = WSTOPSIG(status);
	unsigned event = ((unsigned)status >> 16) & 0xff;

	if (sig == (SIGTRAP | 0x80)) {
		handle_syscall_stop(c, t);
		resume(pid, 0);
	} else if (sig == SIGTRAP && event != 0) {
		handle_ptrace_event(c, t, event);
		resume(pid, 0);
	} else {
		resume(pid, stop_signal_to_deliver(t, sig));
	}
}

static int drain_fd(struct tctx *c, int fd, int stream, struct linebuf *lb,
                    int tty)
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
			continue; /* LCOV_EXCL_LINE: SIGCHLD is blocked via signalfd */
		/* On a pty master, EIO means every slave side is closed: EOF. */
		if (tty && errno == EIO)
			return 0;
		return 1; /* EAGAIN: still open */
	}
}

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Drain the signalfd, then reap every ready tracee stop via waitpid. */
static void reap_children(struct tctx *c, int sfd)
{
	struct signalfd_siginfo si;
	while (read(sfd, &si, sizeof si) > 0)
		;
	pid_t w;
	int ws;
	while ((w = waitpid(-1, &ws, WNOHANG | __WALL)) > 0)
		handle_wait(c, w, ws);
}

/* Pump tracee stops and child output until both run dry. In tty mode there is
 * a single capture fd (the pty master) and one combined stream. */
static void event_loop(struct tctx *c, int sfd, int outfd, int errfd, int tty)
{
	int out_stream = tty ? 0 : 1;
	int out_open = (outfd >= 0);
	int err_open = (errfd >= 0);
	if (outfd >= 0)
		set_nonblock(outfd);
	if (errfd >= 0)
		set_nonblock(errfd);

	while (c->tracees.len > 0 || out_open || err_open) {
		struct pollfd pfd[3];
		int n = 0;
		int isig = -1;
		int iout = -1;
		int ierr = -1;

		if (c->tracees.len > 0) {
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

		if (poll(pfd, (nfds_t)n, -1) < 0) {
			/* LCOV_EXCL_START: poll errors are defensive. */
			if (errno == EINTR)
				continue;
			break;
			/* LCOV_EXCL_STOP */
		}

		if (isig >= 0 && (pfd[isig].revents & POLLIN))
			reap_children(c, sfd);
		if (iout >= 0 && (pfd[iout].revents & (POLLIN | POLLHUP)) &&
		    !drain_fd(c, outfd, out_stream, &c->lb_out, tty))
			out_open = 0;
		if (ierr >= 0 && (pfd[ierr].revents & (POLLIN | POLLHUP)) &&
		    !drain_fd(c, errfd, 2, &c->lb_err, tty))
			err_open = 0;
	}

	lb_flush(c, &c->lb_out, out_stream);
	if (!tty)
		lb_flush(c, &c->lb_err, 2);
}

static void tctx_cleanup(struct tctx *c)
{
	/* LCOV_EXCL_START: the event loop exits only once no tracees remain. */
	for (size_t i = 0; i < c->tracees.len; i++) {
		struct tracee *t = c->tracees.data[i];
		free(t->patha);
		free(t->pathb);
		free(t);
	}
	/* LCOV_EXCL_STOP */
	vec_free(&c->tracees, NULL);
	for (size_t i = 0; i < c->openfds.len; i++) {
		struct openfd *o = c->openfds.data[i];
		free(o->path);
		free(o);
	}
	vec_free(&c->openfds, NULL);
	free(c->lb_out.buf);
	free(c->lb_err.buf);
}

int trace_run(char **argv, struct audit *au, int tty)
{
	int outp[2] = {-1, -1};
	int errp[2] = {-1, -1};
	int master = -1;
	char slave[64];

	if (tty) {
		/* LCOV_EXCL_START: pty allocation failure is defensive. */
		master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (master < 0)
			return -1;
		if (grantpt(master) < 0 || unlockpt(master) < 0 ||
		    ptsname_r(master, slave, sizeof slave) != 0) {
			close(master);
			return -1;
		}
		/* LCOV_EXCL_STOP */
	} else {
		/* LCOV_EXCL_START: pipe setup failure is defensive. */
		if (pipe(outp) || pipe(errp))
			return -1;
		/* LCOV_EXCL_STOP */
	}

	int execp[2];
	/* LCOV_EXCL_START: pipe setup failure is defensive. */
	if (pipe2(execp, O_CLOEXEC)) {
		if (tty)
			close(master);
		else {
			close(outp[0]);
			close(errp[0]);
		}
		return -1;
	}
	/* LCOV_EXCL_STOP */

	sigset_t mask;
	sigset_t old;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, &old);
	int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	/* LCOV_EXCL_START: signalfd failure is defensive. */
	if (sfd < 0) {
		sigprocmask(SIG_SETMASK, &old, NULL);
		return -1;
	}
	/* LCOV_EXCL_STOP */

	pid_t child = fork();
	/* LCOV_EXCL_START: fork failure is defensive. */
	if (child < 0) {
		close(sfd);
		sigprocmask(SIG_SETMASK, &old, NULL);
		return -1;
	}
	/* LCOV_EXCL_STOP */

	/* LCOV_EXCL_START: the child's pre-execvp body cannot record coverage
	 * (gcov counters are discarded across execvp). */
	if (child == 0) {
		/* Child: wire up stdio, announce ourselves to ptrace, exec. On
		 * failure the errno is shipped back over the CLOEXEC pipe. */
		sigprocmask(SIG_SETMASK, &old, NULL);
		if (tty) {
			/* New session with the pty slave as the controlling
		 * terminal: this is what makes isatty(1) true and gives
		 * the child a terminal of its own. */
			setsid();
			int sl = open(slave, O_RDWR);
			if (sl >= 0) {
				dup2(sl, STDIN_FILENO);
				dup2(sl, STDOUT_FILENO);
				dup2(sl, STDERR_FILENO);
				if (sl > STDERR_FILENO)
					close(sl);
			}
			close(master);
		} else {
			dup2(outp[1], STDOUT_FILENO);
			dup2(errp[1], STDERR_FILENO);
			close(outp[0]);
			close(outp[1]);
			close(errp[0]);
			close(errp[1]);
		}
		close(sfd);
		close(execp[0]);
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		execvp(argv[0], argv);
		int e = errno;
		ssize_t w = write(execp[1], &e, sizeof e);
		(void)w;
		_exit(127);
	}
	/* LCOV_EXCL_STOP */

	/* parent */
	if (!tty) {
		/* Without this the parent itself keeps the pipes' write ends
		 * open and EOF never arrives. */
		close(outp[1]);
		close(errp[1]);
	}
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
		if (tty)
			close(master);
		else {
			close(outp[0]);
			close(errp[0]);
		}
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
	/* LCOV_EXCL_START: losing the child before its first stop is defensive. */
	if (waitpid(child, &st, 0) < 0) {
		if (tty)
			close(master);
		else {
			close(outp[0]);
			close(errp[0]);
		}
		close(sfd);
		sigprocmask(SIG_SETMASK, &old, NULL);
		return -1;
	}
	/* LCOV_EXCL_STOP */
	long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK |
	            PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
	            PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
	ptrace(PTRACE_SETOPTIONS, child, 0, (void *)opts);
	struct tracee *ct = add_tracee(&c, child);
	ct->booted = 1;
	resume(child, 0);

	event_loop(&c, sfd, tty ? master : outp[0], tty ? -1 : errp[0], tty);
	tctx_cleanup(&c);

	if (tty)
		close(master);
	else {
		close(outp[0]);
		close(errp[0]);
	}
	close(sfd);
	sigprocmask(SIG_SETMASK, &old, NULL);
	return 0;
}
