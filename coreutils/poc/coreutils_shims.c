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

/* c32iscntrl(): same reasoning as c32isprint()/c32isblank() above -
 * plain ASCII control-character range, the complement of
 * c32isprint()'s own definition (every codepoint is one or the other
 * in a C/POSIX locale). */
int
c32iscntrl(wint_t wc)
{
	return wc < 0x20 || wc == 0x7f;
}

/* c32width(): real gnulib (lib/c32width.c) delegates to libunistring's
 * uc_width() (East-Asian wide-character tables, combining-mark
 * zero-width handling, etc.) - the same large dependency this port
 * isn't pulling in, for the same reason c32isprint() above isn't
 * either. In a C/POSIX locale every character ls -l ever actually
 * measures is plain ASCII: 1 column wide if printable, unreachable
 * (already filtered through quoting - see quotearg.c/quote_name_buf's
 * own use of this exact function) if a control character, which -2
 * (uc_width()'s own documented "undefined width" sentinel gnulib
 * callers already check for) reports honestly rather than guessing. */
#include <uchar.h>

int
c32width(char32_t wc)
{
	return c32isprint((wint_t)wc) ? 1 : -2;
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

/* rmdir(): poc-os has no separate rmdir syscall - musl's own rmdir.c
 * falls back to SYS_unlinkat (a number poc-os doesn't have), so it
 * doesn't compile as shipped. poc-os's own sys_unlink() (kernel/
 * sysfile.c) already removes an empty directory exactly like a real
 * rmdir() would (it explicitly permits a T_DIR inode, checking
 * isdirempty()), so rmdir() is just unlink() under a different name
 * here, not an approximation.
 *
 * rename() no longer needs a shim here at all: it used to be a
 * userspace link()+unlink() emulation (the standard fallback on a
 * filesystem with hard links but no native rename), which could never
 * move a directory since sys_link() correctly refuses to hard-link
 * one. Now that poc-os has a real SYS_rename (kernel/sysfile.c's
 * sys_rename(), include/syscall.h), musl's own real src/stdio/
 * rename.c (in MUSL_LDSO_OBJS - see the Makefile) compiles and works
 * as-is, directory moves included.
 */
#include <unistd.h>

int
rmdir(const char *path)
{
	return unlink(path);
}

/* chmod()/fchmod()/fchmodat(): poc-os's on-disk inode (include/fs.h's
 * struct dinode) now has a real mode field and real SYS_chmod/SYS_fchmod
 * syscalls enforcing it (kernel/sysfile.c's sys_chmod()/sys_fchmod(),
 * owner-or-root) - real syscalls, same raw syscall() pattern as
 * getuid()/setuid() above.
 */
#include <sys/stat.h>
#include <sys/syscall.h>

/* Forward declaration - defined further down in this file, alongside
 * openat()/fstatat()/etc, which is where it conceptually belongs; used
 * here too since fchmodat() needs the same AT_FDCWD-only handling. */
static int require_fdcwd(int dirfd);

int
chmod(const char *path, mode_t mode)
{
	return syscall(SYS_chmod, path, mode);
}

int
fchmod(int fd, mode_t mode)
{
	return syscall(SYS_fchmod, fd, mode);
}

/* lchmod(): chmod() that doesn't follow a trailing symlink - identical
 * to plain chmod() here since poc-os has no symlinks at all (same
 * reasoning as stat()/lstat() above), so there's never a link to not
 * follow.
 */
int
lchmod(const char *path, mode_t mode)
{
	return chmod(path, mode);
}

int
fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
	(void)flags;
	if (require_fdcwd(dirfd) < 0)
		return -1;
	return chmod(path, mode);
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

/* access()/faccessat(): poc-os has no SYS_access/SYS_faccessat, so this
 * reduces to an open()+close() probe - real permission bits now back
 * that probe (see chmod()/kernel/fs.c's permcheck()), so W_OK opens for
 * write (the one real permission open()'s own R/W distinction can
 * actually test) rather than always probing read-only regardless of
 * amode, which would silently misreport write access now that it's a
 * real, enforced thing rather than always true. X_OK has no equivalent
 * "open for execute" mode to probe with - poc-os's own permission model
 * (kernel/fs.c's permcheck()) ties execute to the same owner/group/
 * other bits R/W already use, so an F_OK/R_OK-style existence probe is
 * the closest approximation available without a real access(2) syscall.
 */
#include <fcntl.h>

int
access(const char *path, int amode)
{
	int fd;

	if ((fd = open(path, (amode & W_OK) ? O_WRONLY : O_RDONLY)) < 0)
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

/* openat()/fstatat()/unlinkat()/renameat()/linkat()/readlinkat():
 * poc-os has no *at()-family syscalls at all (no kernel concept of
 * resolving a path relative to an arbitrary open directory fd) - every
 * one of these reduces to its plain, path-based equivalent when dirfd
 * is AT_FDCWD, which is the only case gnulib's fts.c/openat.c actually
 * need here: poc-os's cwd is always effectively "/" (see getcwd()
 * above), and nothing in this port ever opens a directory purely to
 * pass its fd to one of these the way a real race-free, chdir-avoiding
 * implementation would. A real dirfd (anything but AT_FDCWD) has no
 * way to resolve a relative path against it without genuine per-fd
 * path tracking poc-os doesn't have, so that case fails with ENOTSUP
 * rather than silently doing the wrong thing.
 */
#include <stdarg.h>
#include <errno.h>

static int
require_fdcwd(int dirfd)
{
	if (dirfd != AT_FDCWD) {
		errno = ENOTSUP;
		return -1;
	}
	return 0;
}

int
openat(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (require_fdcwd(dirfd) < 0)
		return -1;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return open(path, flags, mode);
}

int
fstatat(int dirfd, const char *restrict path, struct stat *restrict st, int flags)
{
	(void)flags;  /* AT_SYMLINK_NOFOLLOW is moot: poc-os has no symlinks */
	if (require_fdcwd(dirfd) < 0)
		return -1;
	return stat_via_fd(path, st);
}

int
unlinkat(int dirfd, const char *path, int flags)
{
	if (require_fdcwd(dirfd) < 0)
		return -1;
	return (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int
renameat(int olddirfd, const char *old, int newdirfd, const char *new)
{
	if (require_fdcwd(olddirfd) < 0 || require_fdcwd(newdirfd) < 0)
		return -1;
	return rename(old, new);
}

int
linkat(int olddirfd, const char *old, int newdirfd, const char *new, int flags)
{
	(void)flags;
	if (require_fdcwd(olddirfd) < 0 || require_fdcwd(newdirfd) < 0)
		return -1;
	return link(old, new);
}

ssize_t
readlinkat(int dirfd, const char *restrict path, char *restrict buf, size_t bufsize)
{
	if (require_fdcwd(dirfd) < 0)
		return -1;
	return readlink(path, buf, bufsize);
}

/* symlinkat(): poc-os has no symlink support at all (no SYS_symlink,
 * nothing at the filesystem level) - always reporting ENOSYS is the
 * accurate answer here (a real symlink(2)/symlinkat(2) genuinely
 * doesn't exist on this kernel), not a stand-in for a real failure
 * mode.
 */
int
symlinkat(const char *target, int dirfd, const char *path)
{
	(void)target; (void)dirfd; (void)path;
	errno = ENOSYS;
	return -1;
}

/* mkdirat()/mknodat()/fchownat(): same AT_FDCWD-only reasoning as
 * openat()/fstatat()/etc above - poc-os has no *at()-family syscalls,
 * and nothing here ever opens a directory purely to pass its fd to
 * one of these. (lchmodat()/lchownat() themselves need no shim at all -
 * coreutils/lib/openat.h already provides them as real inline wrappers
 * around fchmodat()/fchownat(), which is exactly why both of those,
 * not lchmodat/lchownat by name, are what actually need a poc-os
 * implementation here.) AT_SYMLINK_NOFOLLOW is moot either way: poc-os
 * has no symlinks, so there is never a link for it to *not* follow.
 * mknodat() in particular backs mkfifoat() (coreutils/lib/mkfifoat.c,
 * a real gnulib inline wrapper) - poc-os has a real, working native
 * mknod() (SYS_mknod, unlike mkdir/unlink/link's fellow non-"at"
 * syscalls, this one poc-os actually implements directly), so this is
 * a real translation, not a stub.
 */
int
mkdirat(int dirfd, const char *path, mode_t mode)
{
	if (require_fdcwd(dirfd) < 0)
		return -1;
	return mkdir(path, mode);
}

int
mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
	if (require_fdcwd(dirfd) < 0)
		return -1;
	return mknod(path, mode, dev);
}

int
fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags)
{
	(void)flags;
	if (require_fdcwd(dirfd) < 0)
		return -1;
	return chown(path, owner, group);
}

/* geteuid()/getuid()/getegid()/getgid()/setuid()/seteuid()/setgid()/
 * setegid()/umask(): poc-os now has real per-process identity (include/
 * proc.h's uid/gid/euid/egid/suid/sgid/umask fields) and real syscalls
 * backing every one of these (include/syscall.h's SYS_getuid and
 * friends, kernel/sysproc.c) - direct raw syscall() calls, the same
 * pattern __fstat() above already uses (via __syscall2 rather than a
 * public syscall() wrapper, but the same underlying poc-os ABI), rather
 * than pulling in real musl's own src/unistd/getuid.c/setuid.c/etc:
 * this port prefers a small hand-written shim over growing the
 * Makefile's MUSL_LDSO_OBJS whitelist for functions this simple (see
 * getpwnam()'s own comment further down for the same reasoning applied
 * to /etc/passwd parsing).
 */
#include <unistd.h>
#include <sys/syscall.h>

uid_t
geteuid(void)
{
	return syscall(SYS_geteuid);
}

uid_t
getuid(void)
{
	return syscall(SYS_getuid);
}

gid_t
getegid(void)
{
	return syscall(SYS_getegid);
}

gid_t
getgid(void)
{
	return syscall(SYS_getgid);
}

int
setuid(uid_t uid)
{
	return syscall(SYS_setuid, uid);
}

int
seteuid(uid_t uid)
{
	return syscall(SYS_seteuid, uid);
}

int
setgid(gid_t gid)
{
	return syscall(SYS_setgid, gid);
}

int
setegid(gid_t gid)
{
	return syscall(SYS_setegid, gid);
}

mode_t
umask(mode_t mask)
{
	return syscall(SYS_umask, mask);
}

/* getrandom(): poc-os has no hardware RNG or entropy source of any
 * kind wired up (no SYS_getrandom). Real gnulib callers (e.g. gnulib's
 * hash table seed) already treat getrandom() failing as an ordinary,
 * expected fallback path (weaker seeding, not fatal) - not something
 * this stub needs to paper over with fake randomness.
 */
#include <sys/random.h>

ssize_t
getrandom(void *buf, size_t buflen, unsigned flags)
{
	(void)buf; (void)buflen; (void)flags;
	errno = ENOSYS;
	return -1;
}

/* clock()/clock_gettime(): poc-os has no real-time or per-process-CPU-
 * time clock of any kind wired up to musl (SYS_uptime, a raw timer-
 * tick count since boot, is poc-os's own native concept, not
 * something any real clock_gettime()/clock() caller expects). Most
 * gnulib callers do handle either of these failing as an ordinary
 * fallback path (e.g. skipping a duration measurement) - but not all:
 * coreutils/lib/gettime.c's own gettime() calls clock_gettime() and
 * unconditionally uses *ts right after, never checking the return
 * value at all (ls -l is what actually surfaced this - the first
 * caller in this port that ever reached gettime()). Zeroing *ts
 * before reporting failure - a defined, safe answer (the Unix epoch)
 * instead of whatever garbage happened to be on the caller's stack -
 * is what turns that gnulib bug from a real crash (a wild timestamp
 * feeding a date computation that can walk off an array) into a
 * merely-wrong-looking one (every file's time shown as 1970-01-01),
 * which is the honest degradation a "no real-time clock" answer
 * should have looked like in the first place.
 */
#include <time.h>

clock_t
clock(void)
{
	return (clock_t)-1;
}

int
clock_gettime(clockid_t id, struct timespec *ts)
{
	(void)id;
	if (ts) {
		ts->tv_sec = 0;
		ts->tv_nsec = 0;
	}
	errno = ENOSYS;
	return -1;
}

/* is_selinux_enabled()/getfilecon()/lgetfilecon()/setfscreatecon()/
 * freecon()/selabel_open()/selabel_close(): poc-os has no SELinux of
 * any kind - see coreutils/poc/selinux/{selinux.h,label.h}'s own
 * comment for the full explanation. is_selinux_enabled() always
 * returning 0 is what actually matters (every caller in mkdir.c/mv.c/
 * cp.c/stat.c gates its SELinux-specific code behind
 * "is_selinux_enabled() > 0" first); the rest exist purely so those
 * call sites still link, and fail loudly (ENOTSUP/nullptr) if that
 * invariant is ever violated.
 */
#include <errno.h>

struct selabel_handle;  /* opaque - see coreutils/poc/selinux/label.h */

int
is_selinux_enabled(void)
{
	return 0;
}

int
getfilecon(const char *path, char **con)
{
	(void)path; (void)con;
	errno = ENOTSUP;
	return -1;
}

int
lgetfilecon(const char *path, char **con)
{
	(void)path; (void)con;
	errno = ENOTSUP;
	return -1;
}

int
setfscreatecon(const char *context)
{
	(void)context;
	errno = ENOTSUP;
	return -1;
}

void
freecon(char *con)
{
	(void)con;
}

struct selabel_handle *
selabel_open(unsigned int backend, void *options, unsigned nopts)
{
	(void)backend; (void)options; (void)nopts;
	errno = ENOTSUP;
	return 0;
}

void
selabel_close(struct selabel_handle *handle)
{
	(void)handle;
}

/* getfilecon_raw()/lgetfilecon_raw()/lsetfilecon_raw()/
 * setfscreatecon_raw()/selabel_lookup_raw(): real libselinux's "_raw"
 * suffix means "skip translation between the on-disk raw context and
 * a human-readable translated one" - a distinction that only matters
 * once real security contexts exist to translate, which poc-os never
 * has (same reasoning as every other selinux stub above). */
int
getfilecon_raw(const char *path, char **con)
{
	(void)path; (void)con;
	errno = ENOTSUP;
	return -1;
}

int
lgetfilecon_raw(const char *path, char **con)
{
	(void)path; (void)con;
	errno = ENOTSUP;
	return -1;
}

int
lsetfilecon_raw(const char *path, const char *con)
{
	(void)path; (void)con;
	errno = ENOTSUP;
	return -1;
}

int
setfscreatecon_raw(const char *context)
{
	(void)context;
	errno = ENOTSUP;
	return -1;
}

int
selabel_lookup_raw(struct selabel_handle *handle, char **con, const char *key, int type)
{
	(void)handle; (void)con; (void)key; (void)type;
	errno = ENOTSUP;
	return -1;
}

/* umask(): now a real syscall - see this file's getuid()/setuid() block
 * further up, which defines the real implementation (kernel/sysproc.c's
 * sys_umask(), a genuine per-process mask now applied in kernel/
 * sysfile.c's create()). This used to be a process-wide static stub
 * here since poc-os's on-disk inode had no permission-bits field at
 * all to mask in the first place - no longer true, see include/fs.h's
 * struct dinode. */

/* raise(): musl's own raise.c issues a raw SYS_tkill poc-os has no
 * number for (real POSIX signal delivery, which this kernel has none
 * of - see abort()'s own comment far above). The callers that reach
 * this in practice (e.g. coreutils/lib/savewd.c's signal-safe restore
 * path) only ever raise() a fatal signal to terminate the process
 * itself, which poc-os's own kill() already does natively - kill()
 * targeting our own pid is the closest real equivalent this kernel
 * has to "deliver this signal to myself".
 */
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>

int
raise(int sig)
{
	return kill(getpid(), sig);
}

/* waitpid(): musl's own waitpid.c issues a raw SYS_wait4 poc-os has no
 * number for - poc-os's own wait() (kernel/proc.c) only reaps *some*
 * exited child, with no way to ask for a specific pid or to report a
 * real exit status. When only one child is outstanding (true for
 * every caller reachable here - none of these coreutils spawn more
 * than one subprocess concurrently), looping poc-os's own wait()
 * until it returns the pid asked for is exactly equivalent to a real
 * waitpid(pid, ...); status is always reported as a plain successful
 * exit (0) since poc-os's wait() has no real status to forward -
 * every caller here only checks *whether* the child exited, never how.
 */
#include "syscall.h"

pid_t
waitpid(pid_t pid, int *status, int options)
{
	long w;

	(void)options;
	do {
		w = syscall(SYS_wait);
	} while (pid > 0 && w >= 0 && w != pid);
	if (w < 0)
		return -1;
	if (status)
		*status = 0;
	return w;
}

/* hash_initialize()/hash_lookup()/hash_insert()/hash_remove()/
 * hash_free(): real gnulib (lib/hash.c) is otherwise exactly what's
 * needed here (fts.c/fts-cycle.c's directory-cycle-detection hash
 * table, used by every recursive rm/mv/chmod), but its tuning-
 * parameter validation (check_tuning()) does real `float` arithmetic
 * (epsilon comparisons against growth/shrink thresholds) - a hard
 * compile-time error under this build's -mgeneral-regs-only (every
 * poc-os binary is built that way because the kernel never saves/
 * restores FPU state across a context switch; allowing SSE codegen
 * in just this one file would silently corrupt another process's
 * in-flight float registers under real scheduling pressure, not just
 * fail to compile - so relaxing the flag for this file instead of
 * avoiding float entirely would trade a build error for an
 * intermittent, hard-to-diagnose data-corruption bug).
 *
 * This is a from-scratch, integer-only reimplementation of exactly
 * gnulib's public hash.h surface that fts.c/fts-cycle.c actually call
 * (confirmed: hash_get_first/hash_get_next, the only other exports,
 * are only reachable under fts.c's own GNULIB_FTS_DEBUG, unset here) -
 * same signatures, same insert/lookup/remove semantics (Hash_table
 * stays fully opaque to every caller, exactly like real gnulib's), no
 * tuning parameter at all (a fixed bucket count instead of gnulib's
 * configurable growth/shrink policy) since nothing here ever needs to
 * tune it - fts's own callers always pass tuning=NULL.
 */
#include <stdlib.h>
#include <stdbool.h>

#define POC_HASH_BUCKETS 509  /* prime; fixed, no float-tuned resizing */

struct hash_entry {
	void *data;
	struct hash_entry *next;
};

struct hash_table {
	size_t (*hasher)(const void *, size_t);
	bool (*comparator)(const void *, const void *);
	void (*data_freer)(void *);
	size_t n_entries;
	struct hash_entry *buckets[POC_HASH_BUCKETS];
};

typedef struct hash_table Hash_table;

Hash_table *
hash_initialize(size_t candidate, const void *tuning,
                 size_t (*hasher)(const void *, size_t),
                 bool (*comparator)(const void *, const void *),
                 void (*data_freer)(void *))
{
	Hash_table *t;

	(void)candidate; (void)tuning;
	if (!hasher || !comparator)
		return NULL;
	t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	t->hasher = hasher;
	t->comparator = comparator;
	t->data_freer = data_freer;
	return t;
}

void *
hash_lookup(const Hash_table *table, const void *entry)
{
	size_t idx = table->hasher(entry, POC_HASH_BUCKETS) % POC_HASH_BUCKETS;
	struct hash_entry *e;

	for (e = table->buckets[idx]; e; e = e->next)
		if (table->comparator(e->data, entry))
			return e->data;
	return NULL;
}

void *
hash_insert(Hash_table *table, const void *entry)
{
	void *existing = hash_lookup(table, entry);
	size_t idx;
	struct hash_entry *e;

	if (existing)
		return existing;
	e = malloc(sizeof *e);
	if (!e)
		return NULL;
	e->data = (void *)entry;
	idx = table->hasher(entry, POC_HASH_BUCKETS) % POC_HASH_BUCKETS;
	e->next = table->buckets[idx];
	table->buckets[idx] = e;
	table->n_entries++;
	return (void *)entry;
}

/* hash_get_n_entries(): ls.c's own directory-cycle/inode-seen hash
 * table uses this to check "is it even worth calling hash_lookup()
 * yet" before the table has anything in it - the one other part of
 * gnulib's public hash.h surface (besides hash_get_first/next, see
 * this file's own hash_initialize comment) reachable outside
 * GNULIB_FTS_DEBUG. */
size_t
hash_get_n_entries(const Hash_table *table)
{
	return table->n_entries;
}

void *
hash_remove(Hash_table *table, const void *entry)
{
	size_t idx = table->hasher(entry, POC_HASH_BUCKETS) % POC_HASH_BUCKETS;
	struct hash_entry **pp = &table->buckets[idx];

	while (*pp) {
		if (table->comparator((*pp)->data, entry)) {
			struct hash_entry *dead = *pp;
			void *data = dead->data;

			*pp = dead->next;
			free(dead);
			table->n_entries--;
			return data;
		}
		pp = &(*pp)->next;
	}
	return NULL;
}

void
hash_free(Hash_table *table)
{
	size_t i;

	if (!table)
		return;
	for (i = 0; i < POC_HASH_BUCKETS; i++) {
		struct hash_entry *e = table->buckets[i];

		while (e) {
			struct hash_entry *next = e->next;

			if (table->data_freer)
				table->data_freer(e->data);
			free(e);
			e = next;
		}
	}
	free(table);
}

/* chown()/fchown()/lchown(): poc-os now has real per-inode ownership
 * (include/fs.h's struct dinode) and real SYS_chown/SYS_fchown syscalls
 * enforcing it (kernel/sysfile.c, root-only) - real syscalls, same raw
 * syscall() pattern as getuid()/chmod() above. lchown() doesn't need to
 * differ from chown() for the usual reason (no symlinks at all on this
 * filesystem, so there's never a link to not follow).
 */
int
chown(const char *path, uid_t owner, gid_t group)
{
	return syscall(SYS_chown, path, (int)owner, (int)group);
}

int
fchown(int fd, uid_t owner, gid_t group)
{
	return syscall(SYS_fchown, fd, (int)owner, (int)group);
}

int
lchown(const char *path, uid_t owner, gid_t group)
{
	return chown(path, owner, group);
}

/* sigaction()/sigprocmask()/signal(): poc-os has no signal delivery
 * of any kind (no SYS_rt_sigaction/SYS_rt_sigprocmask, nothing in
 * trap.c that ever raises one in a process's own context - the same
 * fact coreutils_shims.c's raise()/__block_all_sigs() comments
 * already rely on elsewhere). ls -l's job-control handling (SIGINT/
 * SIGTSTP: pause the directory listing cleanly on ^Z, restore the
 * terminal on ^C) is the caller here - reporting every install as
 * having succeeded, with the previous disposition always "default",
 * is the accurate answer for a kernel that will simply never deliver
 * any of these signals to demonstrate otherwise, not a fake success
 * hiding a real gap.
 */
#include <signal.h>

int
sigaction(int sig, const struct sigaction *restrict act,
          struct sigaction *restrict oldact)
{
	(void)sig; (void)act;
	if (oldact) {
		oldact->sa_handler = SIG_DFL;
		oldact->sa_flags = 0;
		sigemptyset(&oldact->sa_mask);
	}
	return 0;
}

int
sigprocmask(int how, const sigset_t *restrict set, sigset_t *restrict oldset)
{
	(void)how; (void)set;
	if (oldset)
		sigemptyset(oldset);
	return 0;
}

/* void(*)(int), not sighandler_t: musl only typedefs sighandler_t
 * under _GNU_SOURCE (musl/include/signal.h), which this build's
 * -D_XOPEN_SOURCE=700 doesn't define. */
void (*signal(int sig, void (*handler)(int)))(int)
{
	(void)sig; (void)handler;
	return SIG_DFL;
}

/* getpwnam()/getpwuid()/getgrnam()/getgrgid()/getpwent(): poc-os now has
 * a real /etc/passwd and /etc/group (colon-separated, classic format -
 * see mkfs/mkfs.c's MKFS_INSTALL and the repo-tracked etc/passwd,
 * etc/group source files themselves). Parsed by hand here (open()+
 * read() into a static buffer, manual ':'-split) rather than via
 * fopen()/fgets() or musl's own real src/passwd/getpwent.c: neither
 * musl/src/stdio/fopen.o nor fgets.o are in the Makefile's
 * MUSL_LDSO_OBJS whitelist today, and musl's real getpwent.c needs its
 * own __getpw_a/__getpwent_a internals (and defines its own competing
 * getpwnam()/getpwuid()) - more machinery than "look up a line in a
 * small file" needs, the same reasoning __fstat()'s own comment already
 * gives for a minimal hand-written shim over growing the whitelist.
 * Not reentrant (static return buffers, shared across all four lookup
 * functions) - matches real getpwnam()/getpwuid()/getgrnam()/getgrgid()'s
 * own non-reentrant contract, and poc-os is single-threaded regardless.
 */
#include <pwd.h>
#include <grp.h>
#include <stdlib.h>

#define PWBUF_SIZE 4096

// Reads path fully into buf (NUL-terminated, truncated if it doesn't
// fit - /etc/passwd and /etc/group are always small). Returns the
// number of bytes read, or -1 if the file couldn't even be opened.
static int
load_file(const char *path, char *buf, int cap)
{
	int fd, n, total = 0;

	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	while (total < cap - 1 && (n = read(fd, buf + total, cap - 1 - total)) > 0)
		total += n;
	close(fd);
	buf[total] = 0;
	return total;
}

// Splits one NUL-terminated "a:b:c:..." line in place (writing NULs
// over the ':' separators) into up to maxfields pointers. Returns the
// number of fields found - a well-formed line yields exactly
// maxfields; anything fewer means a malformed/short line.
static int
split_fields(char *line, char **fields, int maxfields)
{
	int n = 0;
	char *p = line;

	while (n < maxfields) {
		fields[n++] = p;
		p = strchr(p, ':');
		if (!p)
			break;
		*p++ = 0;
	}
	return n;
}

static struct passwd pw_ret;

static struct passwd *
parse_passwd_line(char *line)
{
	char *f[7];

	if (split_fields(line, f, 7) < 7)
		return NULL;
	pw_ret.pw_name = f[0];
	pw_ret.pw_passwd = f[1];
	pw_ret.pw_uid = (uid_t)atoi(f[2]);
	pw_ret.pw_gid = (gid_t)atoi(f[3]);
	pw_ret.pw_gecos = f[4];
	pw_ret.pw_dir = f[5];
	pw_ret.pw_shell = f[6];
	return &pw_ret;
}

struct passwd *
getpwnam(const char *name)
{
	static char buf[PWBUF_SIZE];
	char *line, *nl = NULL;
	struct passwd *pw;

	if (load_file("/etc/passwd", buf, sizeof(buf)) < 0)
		return NULL;
	for (line = buf; line && *line; line = nl ? nl + 1 : NULL) {
		nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		if ((pw = parse_passwd_line(line)) != NULL && strcmp(pw->pw_name, name) == 0)
			return pw;
	}
	return NULL;
}

struct passwd *
getpwuid(uid_t uid)
{
	static char buf[PWBUF_SIZE];
	char *line, *nl = NULL;
	struct passwd *pw;

	if (load_file("/etc/passwd", buf, sizeof(buf)) < 0)
		return NULL;
	for (line = buf; line && *line; line = nl ? nl + 1 : NULL) {
		nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		if ((pw = parse_passwd_line(line)) != NULL && pw->pw_uid == uid)
			return pw;
	}
	return NULL;
}

static struct group gr_ret;
static char *gr_members[9];

static struct group *
parse_group_line(char *line)
{
	char *f[4];
	char *p;
	int n = 0;

	if (split_fields(line, f, 4) < 4)
		return NULL;
	gr_ret.gr_name = f[0];
	gr_ret.gr_passwd = f[1];
	gr_ret.gr_gid = (gid_t)atoi(f[2]);
	p = f[3];
	while (n < 8 && *p) {
		gr_members[n++] = p;
		p = strchr(p, ',');
		if (!p)
			break;
		*p++ = 0;
	}
	gr_members[n] = NULL;
	gr_ret.gr_mem = gr_members;
	return &gr_ret;
}

struct group *
getgrnam(const char *name)
{
	static char buf[PWBUF_SIZE];
	char *line, *nl = NULL;
	struct group *gr;

	if (load_file("/etc/group", buf, sizeof(buf)) < 0)
		return NULL;
	for (line = buf; line && *line; line = nl ? nl + 1 : NULL) {
		nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		if ((gr = parse_group_line(line)) != NULL && strcmp(gr->gr_name, name) == 0)
			return gr;
	}
	return NULL;
}

struct group *
getgrgid(gid_t gid)
{
	static char buf[PWBUF_SIZE];
	char *line, *nl = NULL;
	struct group *gr;

	if (load_file("/etc/group", buf, sizeof(buf)) < 0)
		return NULL;
	for (line = buf; line && *line; line = nl ? nl + 1 : NULL) {
		nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		if ((gr = parse_group_line(line)) != NULL && gr->gr_gid == gid)
			return gr;
	}
	return NULL;
}

/* getpwent(): sequential iteration over /etc/passwd, real semantics -
 * used by nano/src/files.c's real_dir_from_tilde() for ~user expansion.
 * pwent_len==-2 is "not yet loaded this process"; loaded once (poc-os
 * has no setpwent() wired up to force a reload, and nothing in this
 * port needs one - see this file's original comment on the same
 * function for why that's an acceptable scope). */
static char pwent_buf[PWBUF_SIZE];
static int pwent_len = -2;
static int pwent_pos;

struct passwd *
getpwent(void)
{
	char *line, *nl;
	struct passwd *pw;

	if (pwent_len == -2) {
		pwent_len = load_file("/etc/passwd", pwent_buf, sizeof(pwent_buf));
		pwent_pos = 0;
	}
	if (pwent_len < 0)
		return NULL;
	while (pwent_pos < pwent_len && pwent_buf[pwent_pos]) {
		line = pwent_buf + pwent_pos;
		nl = strchr(line, '\n');
		if (nl) {
			*nl = 0;
			pwent_pos = (int)(nl - pwent_buf) + 1;
		} else {
			pwent_pos = pwent_len;
		}
		if ((pw = parse_passwd_line(line)) != NULL)
			return pw;
	}
	return NULL;
}

/* gethostname(): poc-os has no hostname concept at all (no uname(),
 * no configuration for one) - reporting a fixed name is more useful
 * to a caller than failing outright (ls itself never calls this
 * directly, but pulls it in transitively; other callers commonly
 * treat a gethostname() failure as fatal even where the real answer
 * wouldn't matter), the same tradeoff getcwd() above already makes.
 */
int
gethostname(char *name, size_t len)
{
	static const char host[] = "poc-os";

	if (len < sizeof(host)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(name, host, sizeof(host));
	return 0;
}
