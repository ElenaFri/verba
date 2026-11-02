/*
 * verba syscall zoo — a coverage helper, not a standalone test.
 *
 * Run *under verba*, it deterministically performs every file syscall the
 * tracer knows about, in both the plain and the *at spelling, plus a handful of
 * deliberately malformed calls (NULL, bad, and over-long path pointers). Raw
 * syscall(2) is used throughout: the glibc wrappers often emit only the *at
 * form, which would leave the plain-syscall handlers in src/trace.c uncovered.
 *
 * It prints nothing and exits 0; its sole purpose is to make verba's tracer
 * walk down every enter/exit branch. The writes it leaves behind are undeclared
 * (no output mentions them), so verba itself will report findings and exit 1 —
 * which the harness expects.
 *
 * Argument: a scratch directory (must already exist, and must live under the
 * project root so its writes are not swallowed by verba's /tmp whitelist).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static const char *scratch;

/* Build "<scratch>/<name>" into buf. */
static void at(char *buf, size_t n, const char *name)
{
	snprintf(buf, n, "%s/%s", scratch, name);
}

/* Create (and close) a writable file so later ops have a real source. */
static void make_file(const char *path)
{
	long fd = syscall(SYS_open, path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd >= 0)
		(void)syscall(SYS_close, fd);
}

/* open / creat / write / ftruncate / close, plus the malformed-pointer reads. */
static void zoo_open_family(void)
{
	char a[4096];
	long fd;

#ifdef SYS_open
	/* New file: O_CREAT + write intent -> CREATE, OPEN_WRITE, of_add. */
	at(a, sizeof a, "created-by-open");
	fd = syscall(SYS_open, a, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0) {
#ifdef SYS_write
		(void)syscall(SYS_write, fd, "hi", 2); /* ACT_WRITE */
#endif
#ifdef SYS_ftruncate
		(void)syscall(SYS_ftruncate, fd, (off_t)0); /* ACT_TRUNCATE */
#endif
#ifdef SYS_close
		(void)syscall(SYS_close, fd); /* of_remove */
#endif
	}

	/* Existing file, O_TRUNC without O_CREAT -> ACT_TRUNCATE via open exit. */
	fd = syscall(SYS_open, a, O_WRONLY | O_TRUNC);
	if (fd >= 0)
		(void)syscall(SYS_close, fd);

	/* Existing file, O_CREAT but it already exists (existed = 1). */
	fd = syscall(SYS_open, a, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0)
		(void)syscall(SYS_close, fd);

	/* Failed write-open of a missing path -> err && write_intent branch. */
	at(a, sizeof a, "no-such-subdir/file");
	(void)syscall(SYS_open, a, O_WRONLY);

	/* read_string corner cases. */
	(void)syscall(SYS_open, (void *)0, O_WRONLY); /* addr 0: NULL path, then
	                                               * resolve_canon(NULL) and the
	                                               * push_event(NULL) short-circuit */
	(void)syscall(SYS_open, (void *)1, O_CREAT | O_WRONLY, 0644);
	/* ^ bad addr: PEEKDATA faults -> empty path, then path_exists_at("") */
	{
		char longp[600]; /* > 256: forces the read buffer to grow */
		memset(longp, 'a', sizeof longp - 1);
		longp[sizeof longp - 1] = '\0';
		(void)syscall(SYS_open, longp, O_WRONLY);
	}
	{
		char *huge = malloc(5000); /* >= STR_MAX: forces truncation */
		if (huge) {
			memset(huge, 'a', 4999);
			huge[4999] = '\0';
			(void)syscall(SYS_open, huge, O_WRONLY);
			free(huge);
		}
	}
#endif

#ifdef SYS_openat
	/* openat with AT_FDCWD. */
	at(a, sizeof a, "created-by-openat");
	fd = syscall(SYS_openat, AT_FDCWD, a, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0)
		(void)syscall(SYS_close, fd);

	/* openat with a real dirfd + relative path: base_dir(dirfd) branch, and
	 * the failing form drives resolve_canon's relative path on the error exit. */
	long dfd = syscall(SYS_openat, AT_FDCWD, scratch, O_RDONLY | O_DIRECTORY);
	if (dfd >= 0) {
		fd = syscall(SYS_openat, dfd, "rel-at-file", O_CREAT | O_WRONLY,
		             0644);
		if (fd >= 0)
			(void)syscall(SYS_close, fd);
		(void)syscall(SYS_openat, dfd, "no/such/rel", O_WRONLY);
		(void)syscall(SYS_close, dfd);
	}
#endif

#ifdef SYS_creat
	at(a, sizeof a, "created-by-creat");
	fd = syscall(SYS_creat, a, 0644);
	if (fd >= 0)
		(void)syscall(SYS_close, fd);
#endif
}

/* unlink / unlinkat / mkdir / mkdirat. */
static void zoo_unlink_mkdir(void)
{
	char a[4096];

#ifdef SYS_unlink
	at(a, sizeof a, "to-unlink");
	make_file(a);
	(void)syscall(SYS_unlink, a);
#endif
#ifdef SYS_unlinkat
	at(a, sizeof a, "to-unlinkat");
	make_file(a);
	(void)syscall(SYS_unlinkat, AT_FDCWD, a, 0);
#endif
#ifdef SYS_mkdir
	at(a, sizeof a, "mkdir-dir");
	(void)syscall(SYS_mkdir, a, 0755);
#endif
#ifdef SYS_mkdirat
	at(a, sizeof a, "mkdirat-dir");
	(void)syscall(SYS_mkdirat, AT_FDCWD, a, 0755);
#endif
}

/* rename / renameat / renameat2 / link / linkat / symlink / symlinkat. */
static void zoo_rename_link(void)
{
	char a[4096];
	char b[4096];

#ifdef SYS_rename
	at(a, sizeof a, "rename-old");
	at(b, sizeof b, "rename-new");
	make_file(a);
	(void)syscall(SYS_rename, a, b);
#endif
#ifdef SYS_renameat
	at(a, sizeof a, "renameat-old");
	at(b, sizeof b, "renameat-new");
	make_file(a);
	(void)syscall(SYS_renameat, AT_FDCWD, a, AT_FDCWD, b);
#endif
#ifdef SYS_renameat2
	at(a, sizeof a, "renameat2-old");
	at(b, sizeof b, "renameat2-new");
	make_file(a);
	(void)syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, 0u);
#endif
#ifdef SYS_link
	at(a, sizeof a, "link-src");
	at(b, sizeof b, "link-dst");
	make_file(a);
	(void)syscall(SYS_link, a, b);
#endif
#ifdef SYS_linkat
	at(a, sizeof a, "linkat-src");
	at(b, sizeof b, "linkat-dst");
	make_file(a);
	(void)syscall(SYS_linkat, AT_FDCWD, a, AT_FDCWD, b, 0);
#endif
#ifdef SYS_symlink
	at(b, sizeof b, "symlink-link");
	(void)syscall(SYS_symlink, "/some/target", b);
#endif
#ifdef SYS_symlinkat
	at(b, sizeof b, "symlinkat-link");
	(void)syscall(SYS_symlinkat, "/some/target", AT_FDCWD, b);
#endif
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	scratch = argv[1];

	zoo_open_family();
	zoo_unlink_mkdir();
	zoo_rename_link();

	/* Leave one write-opened fd open at exit so verba's cleanup walks the
	 * open-fd table (no close is ever observed for it). */
#ifdef SYS_open
	{
		char lo[4096];
		at(lo, sizeof lo, "left-open");
		(void)syscall(SYS_open, lo, O_CREAT | O_WRONLY, 0644);
	}
#endif

	return 0;
}
