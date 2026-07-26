/*
 * Implements the sysdep hooks listed in include/mlibc/sysdeps.hpp against
 * PoC-OS's actual syscalls (kernel/src/syscall.h). The syscall numbers
 * below are a plain copy of that header's #defines, not a shared
 * include, so they need to stay in sync by hand if kernel/src/syscall.h
 * ever renumbers anything -- deliberate: syscall.h itself isn't written
 * to be includable from a hosted C++ build (it also happens to be tiny
 * and stable, so the sync burden is low).
 */
#include "mlibc/tcb.hpp"
#include <abi-bits/errno.h>
#include <abi-bits/vm-flags.h> /* MAP_FIXED, for Sysdeps<VmMap> below */
#include <bits/syscall.h>
#include <errno.h>
#include <mlibc/all-sysdeps.hpp>
#include <string.h>
#include <stdint.h>
#include <termios.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstddef> /* offsetof, for Sysdeps<ReadEntries> below */

#define POC_NSIG 32

#define SYS_EXIT          1
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_LSEEK         6
#define SYS_FSTAT         7
#define SYS_ANON_ALLOCATE 8
#define SYS_ANON_FREE     9
#define SYS_SET_FS_BASE   10
#define SYS_IOCTL         11
#define SYS_GETPID        12
#define SYS_SIGACTION     13
#define SYS_SIGPROCMASK   14
#define SYS_GETCWD        15
#define SYS_CHDIR         16
#define SYS_CLOCK_GET     17
#define SYS_FORK          18
#define SYS_EXECVE        19
#define SYS_WAITPID       20
#define SYS_PIPE          21
#define SYS_DUP2          22
#define SYS_KILL          24
#define SYS_MKDIR         25
#define SYS_GETPPID       26
#define SYS_MPROTECT             27
#define SYS_ANON_ALLOCATE_FIXED  28
#define SYS_GETDENTS             29
#define SYS_UNLINK               30
#define SYS_RMDIR                31
#define SYS_RENAME               32

/* kernel/src/syscall.h's SYS_IOCTL requests -- console.c is the single
 * global TTY, so these ignore `fd` entirely. */
#define POC_TCGETS    1
#define POC_TCSETS    2
#define POC_TIOCGPGRP 3
#define POC_TIOCSPGRP 4
#define POC_ICANON (1u << 0)
#define POC_ECHO   (1u << 1)

/* mlibc's own <termios.h> (options/posix/include/termios.h) is where
 * unistd.cpp/termios.cpp's sysdep<Ioctl> call sites get these numeric
 * request codes from -- they're pure dispatch tags private to mlibc's
 * generic layer, unrelated to kernel/src/syscall.h's own POC_TIOCGPGRP
 * numbering above (which is what actually crosses the syscall boundary). */
#define MLIBC_TIOCGPGRP 0x540F
#define MLIBC_TIOCSPGRP 0x5410

extern "C" void __pocos_sigreturn_trampoline(void);

#define SYS_PIPE_AGAIN (-2) /* kernel/src/syscall.h: pipe empty/full but not EOF/error -- retry */

namespace mlibc {

void Sysdeps<LibcPanic>::operator()() {
	sysdep<LibcLog>("!!! mlibc panic !!!");
	sysdep<Exit>(-1);
	__builtin_trap();
}

void Sysdeps<LibcLog>::operator()(const char *msg) {
	ssize_t unused;
	sysdep<Write>(2, msg, strlen(msg), &unused);
}

int Sysdeps<Isatty>::operator()(int fd) {
	(void)fd;
	/* Every fd looks like a tty for now -- matches SYS_IOCTL's own
	 * "always succeeds, nothing here actually checks" stance for
	 * anything it doesn't specifically implement (kernel/src/syscall.c). */
	return 0;
}

int Sysdeps<Write>::operator()(int fd, void const *buf, size_t size, ssize_t *ret) {
	long n = syscall(SYS_WRITE, fd, buf, size);
	while (n == SYS_PIPE_AGAIN) {
		n = syscall(SYS_WRITE, fd, buf, size); /* pipe momentarily full, not an error -- retry */
	}
	if (n < 0) {
		return EIO;
	}
	*ret = n;
	return 0;
}

int Sysdeps<TcbSet>::operator()(void *pointer) {
	syscall(SYS_SET_FS_BASE, pointer);
	return 0;
}

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
	long base = syscall(SYS_ANON_ALLOCATE, size);
	if (base == 0) {
		return ENOMEM;
	}
	*pointer = (void *)base;
	return 0;
}

int Sysdeps<AnonFree>::operator()(void *pointer, size_t size) {
	syscall(SYS_ANON_FREE, pointer, size); /* -1 just means it was already gone; nothing more to do */
	return 0;
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
	long ret = syscall(SYS_LSEEK, fd, offset, whence);
	if (ret < 0) {
		return ESPIPE;
	}
	*new_offset = ret;
	return 0;
}

void Sysdeps<Exit>::operator()(int status) {
	syscall(SYS_EXIT, status);
	__builtin_unreachable();
}

int Sysdeps<Close>::operator()(int fd) {
	return (syscall(SYS_CLOSE, fd) == 0) ? 0 : EBADF;
}

pid_t Sysdeps<FutexTid>::operator()() {
	/* Stand-in for "the calling thread's id", used by mlibc's internal
	 * lock implementations to record which thread owns a futex-based
	 * lock -- there are no real threads here (only fork()'d processes,
	 * each with their own address space), so the pid is as good an
	 * identifier as any for "whichever single flow of control is
	 * running in this address space right now". */
	return (pid_t)syscall(SYS_GETPID);
}

int Sysdeps<FutexWake>::operator()(int *, bool) {
	/* This kernel has no threads (only fork()'d processes, each with
	 * their own address space) -- nothing could ever be contending on a
	 * futex within one address space, so waking is always a no-op. */
	return 0;
}

int Sysdeps<FutexWait>::operator()(int *, int, timespec const *) {
	/* Same reasoning as FutexWake -- there's no other thread that could
	 * ever wake this one, so there's nothing to actually block on. */
	return 0;
}

int Sysdeps<Read>::operator()(int fd, void *buf, size_t count, ssize_t *bytes_read) {
	long n = syscall(SYS_READ, fd, buf, count);
	while (n == SYS_PIPE_AGAIN) {
		n = syscall(SYS_READ, fd, buf, count); /* no data yet, but not EOF -- retry */
	}
	if (n < 0) {
		return EIO;
	}
	*bytes_read = n;
	return 0;
}

/* The raw SYS_FSTAT reply (kernel/src/syscall.h's struct poc_stat) --
 * just enough for isatty()/fstat()-driven buffering decisions, not a
 * full POSIX struct stat (no inode numbers, timestamps, link counts).
 * st_mode's format bits already match POSIX's S_IFREG/S_IFDIR/S_IFCHR/
 * S_IFIFO values exactly (see kernel/src/syscall.c's comment), so no
 * translation is needed for that field either. */
struct poc_stat {
	uint64_t st_size;
	uint32_t st_mode;
};

int Sysdeps<Stat>::operator()(mlibc::fsfd_target fsfdt, int fd, const char *path, int flags, struct stat *statbuf) {
	(void)flags;
	int target_fd = fd;
	int opened_fd = -1;
	if (fsfdt == mlibc::fsfd_target::path || fsfdt == mlibc::fsfd_target::fd_path) {
		/* No dirfd-relative opens or symlinks in fat32.c -- treat
		 * fd_path the same as a plain path, resolved against cwd same
		 * as SYS_OPEN always does. */
		long ret = syscall(SYS_OPEN, path, 0 /* O_RDONLY */);
		if (ret < 0) {
			return ENOENT;
		}
		target_fd = opened_fd = (int)ret;
	}

	poc_stat pst;
	long ret = syscall(SYS_FSTAT, target_fd, &pst);
	if (opened_fd >= 0) {
		syscall(SYS_CLOSE, opened_fd);
	}
	if (ret != 0) {
		return ENOENT;
	}

	memset(statbuf, 0, sizeof(*statbuf));
	statbuf->st_size = (off_t)pst.st_size;
	statbuf->st_mode = pst.st_mode;
	statbuf->st_nlink = 1;
	statbuf->st_blksize = 512;
	statbuf->st_blocks = (blkcnt_t)((pst.st_size + 511) / 512);
	return 0;
}

int Sysdeps<Open>::operator()(const char *path, int flags, mode_t mode, int *fd) {
	(void)mode; /* fat32.c has no Unix permission bits to set -- see fat32.h */
	long ret = syscall(SYS_OPEN, path, flags);
	if (ret < 0) {
		return ENOENT;
	}
	*fd = (int)ret;
	return 0;
}

int Sysdeps<VmMap>::operator()(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window) {
	(void)prot; /* every fresh mapping starts RW -- see SYS_ANON_ALLOCATE(_FIXED)'s own doc comment;
	             * the dynamic linker fixes up the real permissions afterwards via VmProtect. */
	(void)offset;
	if (fd != -1) {
		return ENODEV; /* no file-backed mmap -- only anonymous (kernel/src/process.h) */
	}
	long base;
	if (flags & MAP_FIXED) {
		/* The dynamic linker (mlibc/options/rtld) picks its own addresses
		 * for the interpreter's own DSO bump region -- see linker.cpp's
		 * libraryBase -- and asks for exactly that address back, never a
		 * hint it expects us to relocate. */
		base = syscall(SYS_ANON_ALLOCATE_FIXED, (uintptr_t)hint, size);
	} else {
		base = syscall(SYS_ANON_ALLOCATE, size);
	}
	if (base == 0) {
		return ENOMEM;
	}
	*window = (void *)base;
	return 0;
}

int Sysdeps<VmUnmap>::operator()(void *pointer, size_t size) {
	syscall(SYS_ANON_FREE, pointer, size);
	return 0;
}

int Sysdeps<VmProtect>::operator()(void *pointer, size_t size, int prot) {
	long ret = syscall(SYS_MPROTECT, pointer, size, prot);
	if (ret != 0) {
		return EINVAL; /* range not (fully) mapped -- see SYS_MPROTECT's doc comment */
	}
	return 0;
}

int Sysdeps<ClockGet>::operator()(int clock, time_t *secs, long *nanos) {
	(void)clock; /* this kernel has one clock: PIT ticks since boot, not wall-clock time */
	long ticks = syscall(SYS_CLOCK_GET);
	*secs = ticks / 100;
	*nanos = (ticks % 100) * 10000000L;
	return 0;
}

/* --- Phase 3 additions below: process control, signals, cwd, and the
 * handful of termios/ioctl bits a real shell's readline layer touches. --- */

int Sysdeps<Fork>::operator()(pid_t *child) {
	/* SYS_FORK's underlying `int $0x80` genuinely "returns twice" at the
	 * machine level -- process_fork() (kernel/src/process.c) clones the
	 * entire trap frame, so the child resumes right here too, with rax
	 * already 0. No special-casing needed: this function body simply
	 * executes once per side, same as real fork(2). */
	long ret = syscall(SYS_FORK);
	*child = (pid_t)ret;
	return 0;
}

int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags, struct rusage *ru, pid_t *ret_pid) {
	(void)ru; /* no rusage accounting in this kernel */
	long p;
	for (;;) {
		p = syscall(SYS_WAITPID, pid, status);
		if (p != 0) {
			break;
		}
		if (flags & WNOHANG) {
			*ret_pid = 0;
			return 0;
		}
		/* No exited child yet -- SYS_WAITPID never blocks (same
		 * never-block convention as SYS_READ on a pipe), so poll again. */
	}
	if (p < 0) {
		return ECHILD;
	}
	*ret_pid = (pid_t)p;
	return 0;
}

int Sysdeps<Execve>::operator()(const char *path, char *const argv[], char *const envp[]) {
	syscall(SYS_EXECVE, path, argv, envp);
	return ENOEXEC; /* only reached if SYS_EXECVE failed -- it never returns on success */
}

pid_t Sysdeps<GetPid>::operator()() {
	return (pid_t)syscall(SYS_GETPID);
}

pid_t Sysdeps<GetPpid>::operator()() {
	return (pid_t)syscall(SYS_GETPPID);
}

pid_t Sysdeps<GetTid>::operator()() {
	/* No real threads -- same reasoning as FutexTid above. */
	return (pid_t)syscall(SYS_GETPID);
}

uid_t Sysdeps<GetUid>::operator()() { return 1000; }
uid_t Sysdeps<GetEuid>::operator()() { return 1000; }
gid_t Sysdeps<GetGid>::operator()() { return 1000; }
gid_t Sysdeps<GetEgid>::operator()() { return 1000; }

int Sysdeps<Kill>::operator()(pid_t pid, int sig) {
	return (syscall(SYS_KILL, pid, sig) == 0) ? 0 : ESRCH;
}

int Sysdeps<Dup2>::operator()(int fd, int flags, int newfd) {
	(void)flags; /* no O_CLOEXEC-equivalent to apply -- see dup3()'s caller in unistd.cpp */
	return (syscall(SYS_DUP2, fd, newfd) >= 0) ? 0 : EBADF;
}

int Sysdeps<Pipe>::operator()(int *fds, int flags) {
	(void)flags; /* no O_CLOEXEC/O_NONBLOCK-equivalent for pipe2()'s sake */
	return (syscall(SYS_PIPE, fds) == 0) ? 0 : EMFILE;
}

int Sysdeps<GetCwd>::operator()(char *buffer, size_t size) {
	return (syscall(SYS_GETCWD, buffer, size) >= 0) ? 0 : ERANGE;
}

int Sysdeps<Chdir>::operator()(const char *path) {
	return (syscall(SYS_CHDIR, path) == 0) ? 0 : ENOENT;
}

int Sysdeps<Mkdir>::operator()(const char *path, mode_t mode) {
	(void)mode; /* fat32.c has no Unix permission bits to set -- see fat32.h */
	return (syscall(SYS_MKDIR, path) == 0) ? 0 : EIO;
}

int Sysdeps<Rmdir>::operator()(const char *path) {
	return (syscall(SYS_RMDIR, path) == 0) ? 0 : ENOTEMPTY;
}

int Sysdeps<Unlinkat>::operator()(int dirfd, const char *path, int flags) {
	(void)dirfd; /* no dirfd-relative opens in fat32.c/vfs.c -- see Sysdeps<Stat>'s fd_path handling */
	if (flags & AT_REMOVEDIR) {
		return (syscall(SYS_RMDIR, path) == 0) ? 0 : ENOTEMPTY;
	}
	return (syscall(SYS_UNLINK, path) == 0) ? 0 : ENOENT;
}

int Sysdeps<Rename>::operator()(const char *path, const char *new_path) {
	return (syscall(SYS_RENAME, path, new_path) == 0) ? 0 : ENOENT;
}

/* fat32.c has no Unix permission bits to check (see Chmod's own doc
 * comment) -- existence is all `mode` can mean here regardless of
 * whether it's F_OK or some combination of R_OK/W_OK/X_OK, so this
 * just opens and immediately closes the path, same existence-check
 * idiom Sysdeps<Stat> already uses for a path-based query. */
int Sysdeps<Access>::operator()(const char *path, int mode) {
	(void)mode;
	long ret = syscall(SYS_OPEN, path, 0 /* O_RDONLY */);
	if (ret < 0) {
		return ENOENT;
	}
	syscall(SYS_CLOSE, ret);
	return 0;
}

int Sysdeps<Faccessat>::operator()(int dirfd, const char *path, int mode, int flags) {
	(void)dirfd; /* no dirfd-relative opens in fat32.c/vfs.c -- see Sysdeps<Stat>'s fd_path handling */
	(void)flags;
	return Sysdeps<Access>{}(path, mode);
}

/* fat32.c has no mtime/atime fields to set (see Chmod's own doc comment
 * for the same gap with permission bits), so once the target exists
 * this is a silent no-op. But busybox's touch calls this *before*
 * trying to create anything, and only falls back to open(O_CREAT) if
 * it sees ENOENT (see touch.c's own utimensat-then-open logic) -- so
 * unlike Chmod, this can't unconditionally return success: that would
 * make touch believe every nonexistent path already exists and skip
 * creating it. Same open-and-immediately-close existence check
 * Sysdeps<Access> already uses for the same reason. */
int Sysdeps<Utimensat>::operator()(int, const char *path, const struct timespec times[2], int) {
	(void)times;
	long ret = syscall(SYS_OPEN, path, 0 /* O_RDONLY */);
	if (ret < 0) {
		return ENOENT;
	}
	syscall(SYS_CLOSE, ret);
	return 0;
}

/* The raw SYS_GETDENTS reply (kernel/src/syscall.h's struct poc_dirent)
 * -- a fixed-size record (fat32.c's 8.3 names are already bounded to 12
 * characters), unlike the variable-length struct dirent below that this
 * gets translated into. Kept as a plain copy of the kernel header's
 * struct rather than a shared include, same reasoning as poc_stat above. */
struct poc_dirent {
	uint32_t mode;
	char name[13];
};

int Sysdeps<OpenDir>::operator()(const char *path, int *handle) {
	long ret = syscall(SYS_OPEN, path, 0 /* O_RDONLY */);
	if (ret < 0) {
		return ENOENT;
	}
	poc_stat pst;
	if (syscall(SYS_FSTAT, ret, &pst) != 0) {
		syscall(SYS_CLOSE, ret);
		return ENOENT;
	}
	if (!(pst.st_mode & 0040000u) /* S_IFDIR */) {
		syscall(SYS_CLOSE, ret);
		return ENOTDIR;
	}
	*handle = (int)ret;
	return 0;
}

/* Translates up to 32 raw poc_dirent records per call (comfortably under
 * dirent.cpp's own 2048-byte __ent_buffer even at the worst case of every
 * name being the full 12 characters: 32 * (offsetof(dirent,d_name) + 13)
 * is still well under 2048) into real, variable-length struct dirent
 * records -- d_off is set to the *next* record's index (matching
 * readdir()'s "resume from d_off" convention with the SYS_GETDENTS
 * index this same fd's `offset` field already tracks kernel-side, so a
 * subsequent lseek(fd, d_off, SEEK_SET) would resume in the right place,
 * to the (limited -- see process_fd_getdents()'s own doc comment) extent
 * this kernel's directory-fd lseek supports arbitrary offsets at all). */
int Sysdeps<ReadEntries>::operator()(int handle, void *buffer, size_t max_size, size_t *bytes_read) {
	poc_dirent stage[32];
	long n = syscall(SYS_GETDENTS, handle, stage, 32);
	if (n < 0) {
		return EBADF;
	}

	uint8_t *out = (uint8_t *)buffer;
	size_t used = 0;
	for (long i = 0; i < n; i++) {
		size_t name_len = strlen(stage[i].name);
		size_t reclen = offsetof(struct dirent, d_name) + name_len + 1;
		if (used + reclen > max_size) {
			break;
		}
		struct dirent *entp = reinterpret_cast<struct dirent *>(out + used);
		entp->d_ino = 1; /* fat32.c has no inode numbers -- any nonzero value keeps callers that skip d_ino == 0 happy */
		entp->d_off = (off_t)(used + reclen);
		entp->d_reclen = (reclen_t)reclen;
		entp->d_type = (stage[i].mode & 0040000u) ? DT_DIR : DT_REG;
		memcpy(entp->d_name, stage[i].name, name_len + 1);
		used += reclen;
	}
	*bytes_read = used;
	return 0;
}

/* Shadow copy of installed handlers, since kernel/src/syscall.h's
 * SYS_SIGACTION always *sets* a handler (no "just query, don't change"
 * mode the way real sigaction(sig, NULL, &old) needs) -- tracking our own
 * copy here answers oldact queries without needing the kernel's help.
 * Survives fork() correctly: process_fork() copies this static's backing
 * page like any other process memory. */
namespace {
void (*g_sigaction_shadow[POC_NSIG])(int) = {};
}

int Sysdeps<Sigaction>::operator()(int signum, const struct sigaction *__restrict act, struct sigaction *__restrict oldact) {
	if (signum < 0) {
		return EINVAL;
	}
	if (signum >= POC_NSIG) {
		/* ENOSYS (not EINVAL): out of range for POC_NSIG's fixed 32
		 * signals, but not a malformed argument -- SIGCANCEL (32, real-
		 * time-signal range) is exactly this case. options/posix/generic/
		 * pthread.cpp's PthreadSignalInstaller global constructor (linked
		 * into libc.so unconditionally, unlike the static build, where it
		 * simply never gets pulled in unless something references
		 * pthreads) specifically treats ENOSYS from here as "this port
		 * has no cancellation signal support" and opts out gracefully;
		 * any other errno makes it __ensure()-abort instead. */
		return ENOSYS;
	}
	if (oldact) {
		memset(oldact, 0, sizeof(*oldact));
		oldact->sa_handler = g_sigaction_shadow[signum];
	}
	if (act) {
		long ret = syscall(SYS_SIGACTION, signum, (long)act->sa_handler, (long)&__pocos_sigreturn_trampoline);
		if (ret < 0) {
			return EINVAL;
		}
		g_sigaction_shadow[signum] = act->sa_handler;
	}
	return 0;
}

int Sysdeps<Sigprocmask>::operator()(int how, const sigset_t *__restrict set, sigset_t *__restrict retrieve) {
	long prev;
	if (set) {
		prev = syscall(SYS_SIGPROCMASK, how, set->__sig[0]);
	} else {
		/* Query-only: SIG_UNBLOCK with an empty mask changes nothing
		 * (new = current & ~0 = current) but still returns the
		 * previous/current mask, same trick used for a missing `act`
		 * in Sigaction above. */
		prev = syscall(SYS_SIGPROCMASK, 1 /* SIG_UNBLOCK */, 0);
	}
	if (retrieve) {
		memset(retrieve, 0, sizeof(*retrieve));
		retrieve->__sig[0] = (unsigned long)prev;
	}
	return 0;
}

int Sysdeps<Ioctl>::operator()(int fd, unsigned long request, void *arg, int *result) {
	if (request == MLIBC_TIOCGPGRP) {
		uint64_t fg = 0;
		if (syscall(SYS_IOCTL, fd, POC_TIOCGPGRP, &fg) < 0) {
			return ENOTTY;
		}
		*(int *)arg = (int)fg;
		if (result) {
			*result = 0;
		}
		return 0;
	}
	if (request == MLIBC_TIOCSPGRP) {
		int pgrp = *(int *)arg;
		if (syscall(SYS_IOCTL, fd, POC_TIOCSPGRP, (long)pgrp) < 0) {
			return ENOTTY;
		}
		if (result) {
			*result = 0;
		}
		return 0;
	}
	/* TIOCGSID (no session concept in this kernel) and anything else --
	 * every call site (posix_devctl.cpp, termios.cpp's tcgetsid) uses
	 * sysdep_or_enosys, so returning an errno here fails that one POSIX
	 * call gracefully rather than aborting the whole process. */
	return ENOSYS;
}

int Sysdeps<Tcgetattr>::operator()(int fd, struct termios *attr) {
	uint32_t lflag = 0;
	if (syscall(SYS_IOCTL, fd, POC_TCGETS, &lflag) < 0) {
		return ENOTTY;
	}
	memset(attr, 0, sizeof(*attr));
	attr->c_iflag = ICRNL | IXON;
	attr->c_oflag = OPOST;
	attr->c_cflag = CS8 | CREAD;
	attr->c_lflag = ISIG;
	if (lflag & POC_ICANON) {
		attr->c_lflag |= ICANON;
	}
	if (lflag & POC_ECHO) {
		attr->c_lflag |= ECHO;
	}
	attr->c_cc[VMIN] = 1;
	attr->c_cc[VTIME] = 0;
	attr->c_cc[VINTR] = 3; /* ^C */
	attr->c_cc[VEOF] = 4;  /* ^D */
	return 0;
}

int Sysdeps<Tcsetattr>::operator()(int fd, int opts, const struct termios *attr) {
	(void)opts; /* TCSANOW/TCSADRAIN/TCSAFLUSH distinction doesn't matter -- no output buffering to drain/flush */
	uint32_t lflag = 0;
	if (attr->c_lflag & ICANON) {
		lflag |= POC_ICANON;
	}
	if (attr->c_lflag & ECHO) {
		lflag |= POC_ECHO;
	}
	if (syscall(SYS_IOCTL, fd, POC_TCSETS, &lflag) < 0) {
		return ENOTTY;
	}
	return 0;
}

int Sysdeps<Fcntl>::operator()(int fd, int request, va_list args, int *result) {
	switch (request) {
	case F_GETFD:
		*result = 0; /* no close-on-exec flag to report -- this kernel doesn't support one */
		return 0;
	case F_SETFD:
		(void)va_arg(args, int);
		*result = 0;
		return 0;
	case F_GETFL:
		*result = O_RDWR; /* good enough -- nothing here checks O_NONBLOCK/O_APPEND via fcntl */
		return 0;
	case F_SETFL:
		(void)va_arg(args, int);
		*result = 0;
		return 0;
	case F_DUPFD:
	case F_DUPFD_CLOEXEC: {
		/* Not real "lowest free fd >= arg" semantics (this kernel has no
		 * way to query fd-in-use state) -- just dup2 onto the requested
		 * number directly, which is exactly what bash's own callers
		 * (moving a redirection target out of the way) actually want. */
		int min_fd = va_arg(args, int);
		long ret = syscall(SYS_DUP2, fd, min_fd);
		if (ret < 0) {
			return EBADF;
		}
		*result = (int)ret;
		return 0;
	}
	default:
		return ENOSYS;
	}
}

int Sysdeps<Ttyname>::operator()(int fd, char *buf, size_t size) {
	(void)fd; /* one global console -- every fd that isatty()'s true is "it" */
	static const char name[] = "/dev/console";
	if (size < sizeof(name)) {
		return ERANGE;
	}
	memcpy(buf, name, sizeof(name));
	return 0;
}

int Sysdeps<Fchdir>::operator()(int) {
	/* No fd-to-path reverse mapping in fat32.c/vfs.c to chdir *to* an
	 * already-open fd -- bash falls back to other means (its own cached
	 * cwd string) when this fails, same as any POSIX caller must. */
	return ENOSYS;
}

int Sysdeps<Fsync>::operator()(int) {
	return 0; /* no write-back caching in fat32.c to flush -- writes are already synchronous */
}

int Sysdeps<Chmod>::operator()(const char *, mode_t) {
	return 0; /* fat32.c has no Unix permission bits to set -- see fat32.h */
}

int Sysdeps<GetHostname>::operator()(char *buf, size_t size) {
	static const char name[] = "pocos";
	if (size < sizeof(name)) {
		return ENAMETOOLONG;
	}
	memcpy(buf, name, sizeof(name));
	return 0;
}

int Sysdeps<Sysconf>::operator()(int num, long *ret) {
	switch (num) {
	case _SC_CLK_TCK:
		*ret = 100; /* matches SYS_CLOCK_GET's own 100Hz PIT tick rate */
		return 0;
	case _SC_PAGE_SIZE:
		*ret = 4096;
		return 0;
	case _SC_OPEN_MAX:
		*ret = 64;
		return 0;
	case _SC_NPROCESSORS_ONLN:
		*ret = 1; /* no SMP */
		return 0;
	case _SC_ARG_MAX:
		*ret = 15; /* matches SYS_EXECVE's own argv/envp entry limit */
		return 0;
	case _SC_CHILD_MAX:
		*ret = 25; /* matches PROCESS_MAX-ish; no real per-parent limit enforced */
		return 0;
	default:
		return EINVAL;
	}
}

/* Bit-tests/clears an fd_set directly (same layout/convention as mlibc's
 * own __FD_ISSET/__FD_ZERO in options/posix/generic/sys-select.cpp)
 * instead of calling those functions -- they live in libc_all_sources,
 * not rtld_sources, so they're simply unavailable to this file when it's
 * compiled into ld.so (a standalone, fully-resolved `-shared` link,
 * unlike the static build where every object ends up in one archive
 * regardless of which "sources" list it came from). */
static int pocos_fd_isset(int fd, const fd_set *set) {
	return set->fds_bits[fd / 8] & (1 << (fd % 8));
}

int Sysdeps<Pselect>::operator()(int num_fds, fd_set *read_set, fd_set *write_set,
                                  fd_set *except_set, const struct timespec *timeout,
                                  const sigset_t *sigmask, int *num_events) {
	/* This kernel has no real event-driven I/O wait -- every fd kind
	 * (console/pipe/file) already reports readiness via its own retry
	 * convention at the SYS_READ/SYS_WRITE level instead (SYS_PIPE_AGAIN,
	 * and now the same sentinel for an empty-but-open console -- see
	 * process_fd_read()'s console_kind==1 case). So rather than actually
	 * polling here, just report every fd the caller asked about as
	 * ready immediately; the real "wait until data shows up" happens one
	 * level down, inside Read/Write's own retry loop, which already
	 * yields to the scheduler (and keyboard IRQs) between attempts. */
	(void)timeout;
	(void)sigmask;
	int count = 0;
	for (int i = 0; i < num_fds; i++) {
		if (read_set && pocos_fd_isset(i, read_set)) {
			count++;
		}
		if (write_set && pocos_fd_isset(i, write_set)) {
			count++;
		}
	}
	if (except_set) {
		memset(except_set->fds_bits, 0, sizeof(fd_set));
	}
	*num_events = count;
	return 0;
}

int Sysdeps<GetGroups>::operator()(size_t, gid_t *, int *ret) {
	/* No supplementary groups in this kernel's single-user model (see
	 * GetUid/GetGid above) -- always report zero, regardless of the
	 * caller's buffer size. */
	*ret = 0;
	return 0;
}

int Sysdeps<SetPgid>::operator()(pid_t, pid_t) {
	/* No real process groups in this kernel (console.c's single
	 * foreground-pid concept only -- see the plan's "minimal job
	 * control" scope) -- called even with JOB_CONTROL off (bash always
	 * tries to put itself in its own process group at startup), so this
	 * just succeeds as a no-op rather than aborting. */
	return 0;
}

int Sysdeps<Tcdrain>::operator()(int) { return 0; }
int Sysdeps<Tcflush>::operator()(int, int) { return 0; }
int Sysdeps<Tcflow>::operator()(int, int) { return 0; }
int Sysdeps<Tcsendbreak>::operator()(int, int) { return 0; }

} // namespace mlibc
