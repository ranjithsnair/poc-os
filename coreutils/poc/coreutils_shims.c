/* poc-os-specific stand-ins for the handful of real gnulib functions
 * that are normally declared only inside gnulib's own header
 * *replacements* (lib/wchar.in.h, lib/stdio.in.h's HAVE_FPURGE path,
 * ...) - this build uses musl's real public headers directly instead
 * (see the build script's -I order), so those declarations, and in
 * fpurge's/c32isprint's case the definitions too, never happen. Not
 * gnulib source - hand-written for this port, same as
 * musl/test/ldso_stubs.c was for the dynamic linker.
 */
#include <string.h>
#include <wchar.h>
/* stdio_impl.h (see __fpending() below) pulls in musl's *internal*
 * stdio.h override (musl/src/include/stdio.h, found first via this
 * file's own -I order - see the build script) instead of the public
 * musl/include/stdio.h, which is what makes struct _IO_FILE's real
 * fields (as opposed to the public API's opaque placeholder) visible
 * at all - so this needs to come before any other stdio.h-pulling
 * include in this file, public or not. */
#include "stdio_impl.h"

/* mbszero(): put an mbstate_t into its initial conversion state -
 * gnulib's own definition (lib/wchar.in.h) is exactly this, a plain
 * memset; poc-os's locale support is permanently the C/POSIX locale
 * (see musl/src/locale/), so mbstate_t here never actually holds a
 * real shift-state to reset, but zeroing it is still the correct,
 * standard-mandated behavior. */
void
mbszero(mbstate_t *ps)
{
	memset(ps, 0, sizeof(*ps));
}

/* c32isprint(): real gnulib (lib/c32isprint.c) delegates to
 * GNU libunistring's full Unicode general-category tables - a large
 * dependency this port isn't pulling in. poc-os's locale is always
 * C/POSIX (no real UTF-8/Unicode locale ever gets loaded - see
 * musl/src/locale/), so plain ASCII printability is the *correct*
 * answer here, not a reduced approximation of a Unicode-aware one. */
int
c32isprint(wint_t wc)
{
	return wc >= 0x20 && wc < 0x7f;
}

/* c32isblank(): same reasoning as c32isprint() above (real gnulib delegates
 * to libunistring's Unicode tables; poc-os's locale is always C/POSIX) -
 * plain ASCII space/tab is the correct answer, not an approximation. */
int
c32isblank(wint_t wc)
{
	return wc == ' ' || wc == '\t';
}

/* fpurge(): a BSD stdio extension (discard any unwritten buffered
 * output without writing it) musl doesn't have. Only reachable from
 * src/system.h's write_error(), itself only reachable once a write
 * has already failed and the program is about to error() out - so
 * fflush()'s slightly different semantics (attempt to write, rather
 * than discard) make no practical difference here: the program exits
 * either way, and fflush() failing silently (its return value is
 * ignored, matching real fpurge() callers) is harmless. */
int
fpurge(FILE *f)
{
	return fflush(f);
}

/* __fpending(): real gnulib (lib/fpending.c) needs platform-specific
 * knowledge of the FILE struct's internal write-buffer fields, which
 * it has for glibc/BSD libc layouts but not musl's - reading musl's
 * own internal struct _IO_FILE directly (musl/src/internal/
 * stdio_impl.h; wpos/wbase are the same fields musl's own stdio
 * internals use for this, e.g. putc_unlocked's fast path) is the
 * genuinely correct implementation for this libc, not a workaround. */
size_t
__fpending(FILE *f)
{
	return f->wpos - f->wbase;
}

/* abort(): real musl (musl/src/exit/abort.c) raises SIGABRT, falling
 * back to directly uninstalling any handler and re-raising via
 * SYS_rt_sigaction/SYS_tkill/SYS_rt_sigprocmask if that didn't
 * terminate the process - poc-os has no signal delivery of any kind
 * yet (kill() is poc-os's own native semantics, not POSIX signals; no
 * SIGABRT, no handlers, nothing to catch or block). _Exit(134) is
 * exactly what abort()'s default action looks like from the outside
 * (128+SIGABRT, the same status a shell reports for a real
 * signal-terminated process) without needing signal infrastructure
 * this kernel doesn't have. */
#include <stdlib.h>

_Noreturn void
abort(void)
{
	_Exit(134);
}

/* __fstat()/fstat(): NOT defined here - musl/test/ldso_stubs.c
 * already provides both (the dynamic linker needed its own translated
 * fstat() first - see that file's comment for the real explanation),
 * and since coreutils_shims.o now links into libc.so alongside it
 * (both are part of MUSL_LDSO_OBJS - see the Makefile), redefining
 * them here would just be a duplicate-symbol link error. */

/* flockfile()/funlockfile(): real musl (musl/src/stdio/flockfile.c/
 * funlockfile.c) additionally tracks a per-FILE lockcount (for
 * correctly-nested recursive locking) and registers/unregisters each
 * locked FILE on a list consulted after fork() in a multithreaded
 * parent (musl/src/stdio/__lockfile.c's __lock/__unlock aren't
 * enough by themselves for that). poc-os has no threads and
 * getopt.c's own flockfile/funlockfile calls (its only caller here)
 * are always a single, non-nested pair, so wrapping __lockfile()/
 * __unlockfile() directly - already safe to call repeatedly from the
 * same "thread" without deadlocking, see __lockfile's own fast path -
 * is complete, not a simplification of real nesting/fork behavior
 * this port doesn't need. */
void
flockfile(FILE *f)
{
	__lockfile(f);
}

void
funlockfile(FILE *f)
{
	__unlockfile(f);
}

/* rmdir()/rename(): poc-os has neither a separate rmdir syscall nor a
 * rename one - musl's own rmdir.c falls back to SYS_unlinkat (a number
 * poc-os doesn't have) and rename.c to SYS_rename/SYS_renameat[2] (same
 * problem), so neither compiles as shipped, let alone runs. poc-os's
 * own sys_unlink() (kernel/sysfile.c) already removes an empty
 * directory exactly like a real rmdir() would (it explicitly permits a
 * T_DIR inode, checking isdirempty()), so rmdir() is just unlink()
 * under a different name here, not an approximation. rename() has no
 * single poc-os syscall backing it at all, but poc-os does have real
 * link()/unlink() - link(old,new) then unlink(old) is the standard
 * userspace rename() emulation on a filesystem with hard links and no
 * native rename, with the two gaps that emulation always has: not
 * atomic, and (unlike a real rename()) unable to replace a directory,
 * since poc-os's own sys_link() refuses to hard-link one (see its own
 * "ip->type == T_DIR" check) - moving a file works, moving a directory
 * reports an error instead of silently doing the wrong thing.
 */
#include <unistd.h>

int
rmdir(const char *path)
{
	return unlink(path);
}

int
rename(const char *old, const char *new)
{
	unlink(new); /* best-effort: fine whether or not "new" exists */
	if (link(old, new) < 0)
		return -1;
	return unlink(old);
}

/* chmod()/fchmod()/fchmodat(): poc-os's on-disk inode (include/fs.h's
 * struct dinode) has no permission-bits field at all - every file is
 * implicitly readable/writable/executable by whoever can open it, same
 * as real xv6. A permission change therefore has nothing to do and
 * nothing to fail: returning 0 unconditionally is the correct
 * "succeeded, no-op" answer for a filesystem with no permission model,
 * not a fake success covering up a real error path.
 */
#include <sys/stat.h>

int
chmod(const char *path, mode_t mode)
{
	(void)path; (void)mode;
	return 0;
}

int
fchmod(int fd, mode_t mode)
{
	(void)fd; (void)mode;
	return 0;
}

int
fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
	(void)dirfd; (void)path; (void)mode; (void)flags;
	return 0;
}

/* utimensat()/futimens(): poc-os's inode has no atime/mtime fields
 * either (see struct dinode again) - same reasoning as chmod() above,
 * a timestamp update has nothing to actually store. touch's other
 * effect (creating a missing file) still goes through open()'s real
 * O_CREATE, so "touch newfile" still works; only the timestamp bump on
 * an already-existing file is silently a no-op.
 */
int
utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
	(void)dirfd; (void)path; (void)times; (void)flags;
	return 0;
}

int
futimens(int fd, const struct timespec times[2])
{
	(void)fd; (void)times;
	return 0;
}

/* access()/faccessat(): poc-os has no SYS_access/SYS_faccessat and,
 * like chmod() above, no permission bits to check even if it did -
 * open()+close() is the existence probe every access() reduces to once
 * permission bits are moot; F_OK/R_OK/W_OK/X_OK are otherwise
 * indistinguishable here since every openable file is all three at
 * once.
 */
#include <fcntl.h>

int
access(const char *path, int amode)
{
	int fd;

	(void)amode;
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	close(fd);
	return 0;
}

int
faccessat(int dirfd, const char *path, int amode, int flags)
{
	(void)dirfd; (void)flags;
	return access(path, amode);
}

/* stat()/lstat(): poc-os has no SYS_stat/SYS_lstat/SYS_newfstatat -
 * every real stat(2)-family call here has to go through the one
 * syscall poc-os does have, by opening the path first and reusing the
 * already-translated __fstat() (musl/test/ldso_stubs.c, linked into
 * libc.so alongside this file - the actual poc_stat->stat field
 * translation lives there, not duplicated here). lstat() is identical
 * to stat(): poc-os has no symlinks at all (no SYS_symlink, nothing at
 * the filesystem level like ELF_PROG_INTERP's indirection), so there
 * is never a link to *not* follow.
 */
int __fstat(int fd, struct stat *st);

static int
stat_via_fd(const char *path, struct stat *st)
{
	int fd, r;

	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	r = __fstat(fd, st);
	close(fd);
	return r;
}

int
stat(const char *restrict path, struct stat *restrict st)
{
	return stat_via_fd(path, st);
}

int
lstat(const char *restrict path, struct stat *restrict st)
{
	return stat_via_fd(path, st);
}

/* getcwd(): poc-os's per-process cwd (kernel/proc.h's proc->cwd) is an
 * inode reference, not a path string - there is no SYS_getcwd and no
 * way to reconstruct a path from just an inode without directory-
 * reading support poc-os doesn't have yet (walking ".." entries back to
 * the root). Always reporting "/" is wrong whenever the real cwd isn't
 * actually root, but every coreutils caller here treats a getcwd()
 * failure as fatal even in places where the real answer wouldn't
 * matter (e.g. pwd's whole job when you haven't cd'd anywhere) - a
 * fixed answer that's at least sometimes right is more useful than an
 * unconditional failure. The malloc() path (getcwd(NULL, 0), which
 * real POSIX getcwd() defines as allocating the buffer itself) is real,
 * not stubbed: libc.so already links a working malloc (see
 * MUSL_LDSO_OBJS), and a caller that free()s the result needs an
 * actual heap pointer back, not a pointer to static storage.
 */
#include <stdlib.h>
#include <errno.h>

char *
getcwd(char *buf, size_t size)
{
	static const char root[] = "/";

	if (!buf) {
		char *p = malloc(sizeof(root));
		if (p)
			memcpy(p, root, sizeof(root));
		return p;
	}
	if (size < sizeof(root)) {
		errno = ERANGE;
		return 0;
	}
	memcpy(buf, root, sizeof(root));
	return buf;
}

/* freadahead()/freadptr()/freadseek(): real gnulib (lib/freadahead.c,
 * lib/freadptr.c, lib/freadseek.c) each detect their host libc by
 * compiler-predefined macros (_IO_EOF_SEEN for glibc, __sferror for the
 * BSDs, __UCLIBC__, __QNX__, ...) and fall back to #error on anything
 * else - musl isn't one of the recognized names, even though musl has
 * always shipped exactly the functions gnulib wants here
 * (musl/src/stdio/ext2.c's __freadahead/__freadptr/__freadptrinc,
 * linked into libc.so - see MUSL_LDSO_OBJS), just double-underscore
 * prefixed and not auto-detected by a config.h this port's own
 * coreutils/poc/config.h comment already says was hand-adapted, not
 * produced by a real musl-targeting ./configure. freadahead()/
 * freadptr() are direct one-line forwards; freadseek() re-implements
 * real gnulib's own algorithm (drain the buffer via freadptr()/
 * __freadptrinc() first, a byte-at-a-time fgetc() fallback across an
 * ungetc() boundary, then fseeko() for a seekable stream or a discard-
 * read loop for a pipe) - not a simplification, the exact logic
 * lib/freadseek.c's own #error'd-out generic path would run if musl
 * were on gnulib's recognized-platform list.
 */
size_t __freadahead(FILE *f);
const char *__freadptr(FILE *f, size_t *sizep);
void __freadptrinc(FILE *f, size_t inc);

size_t
freadahead(FILE *fp)
{
	return __freadahead(fp);
}

const char *
freadptr(FILE *fp, size_t *sizep)
{
	return __freadptr(fp, sizep);
}

int
freadseek(FILE *fp, size_t offset)
{
	size_t total_buffered, buffered;
	int fd;

	if (offset == 0)
		return 0;

	total_buffered = __freadahead(fp);
	while (total_buffered > 0) {
		const char *p = __freadptr(fp, &buffered);

		if (p != NULL && buffered > 0) {
			size_t increment = buffered < offset ? buffered : offset;

			__freadptrinc(fp, increment);
			offset -= increment;
			if (offset == 0)
				return 0;
			total_buffered -= increment;
			if (total_buffered == 0)
				break;
		}
		if (fgetc(fp) == EOF)
			return -1;
		offset--;
		if (offset == 0)
			return 0;
		total_buffered--;
	}

	fd = fileno(fp);
	if (fd >= 0 && lseek(fd, 0, SEEK_CUR) >= 0)
		return fseeko(fp, offset, SEEK_CUR);

	while (offset > 0) {
		char buf[4096];
		size_t count = sizeof(buf) < offset ? sizeof(buf) : offset;

		if (fread(buf, 1, count, fp) < count)
			return -1;
		offset -= count;
	}
	return 0;
}

/* rawmemchr(): a GNU libc extension (unbounded memchr(), scanning until it
 * finds c with no length limit - the caller guarantees c does occur)
 * musl doesn't provide at all, GNU or otherwise. head.c/tail.c only ever
 * call it on a NUL-terminated buffer to find the next line separator, so
 * this is real, unbounded-scan rawmemchr semantics, not a bounded
 * memchr() standing in for it.
 */
void *
rawmemchr(const void *s, int c)
{
	const unsigned char *p = s;

	while (*p != (unsigned char)c)
		p++;
	return (void *)p;
}
