// System call numbers
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_kill    6
#define SYS_exec    7
#define SYS_fstat   8
#define SYS_chdir   9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_sleep  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
#define SYS_mknod  17
#define SYS_unlink 18
#define SYS_link   19
#define SYS_mkdir  20
#define SYS_close  21
// execve, not exec: takes a third (envp) argument and, unlike exec(),
// hands the new program a Linux-style argc/argv/envp/auxv stack (see
// kernel/exec.c's execve()) instead of poc-os's native argc-in-%rdi/
// argv-in-%rsi convention - musl's crt1 (_start in
// musl/arch/x86_64/crt_arch.h) expects to find argc at the initial
// %rsp, not in registers. Kept as a separate syscall from SYS_exec
// rather than replacing it so every existing native xv6 binary (built
// with poc-os's own ulib.c, entered directly at main()) keeps working
// unchanged.
#define SYS_execve 22
// (code, addr) - only ARCH_SET_FS (see include/mmu.h) is implemented;
// used by musl's __set_thread_area (musl/src/thread/x86_64/
// __set_thread_area.s, forked for poc-os the same way syscall_arch.h
// was) to install the thread pointer every musl program needs even
// single-threaded, since errno and friends are TLS-relative.
#define SYS_arch_prctl 23
// (addr, len, prot, flags, fd, offset). Every mapping still lives
// inside the single contiguous [0, proc->sz) region poc-os has always
// used for a process's whole address space (there's no separate VMA
// list) - "mmap" here means "grow sz and populate the new pages",
// exactly like the brk-style anonymous case always did. addr==0 (or
// any addr >= the current sz) always grows sz and returns the new
// region's base; addr < sz (MAP_FIXED, per a real dynamic linker's
// "reserve with one mmap, then MAP_FIXED sub-mmap each segment into
// it" pattern - see musl/ldso/dynlink.c's map_library()) overlays new
// content/permissions onto an already-mapped range instead of growing
// sz further. fd != -1 reads the mapping's content from that file at
// offset; fd == -1 zero-fills it (allocuvm() already zero-fills any
// newly grown page regardless). See kernel/sysproc.c's sys_mmap().
#define SYS_mmap   24
// (addr, len). Only succeeds when [addr, addr+len) is exactly the
// current top of the address space (what sys_mmap() above always
// returns) - poc-os has no VMA list to support unmapping an arbitrary
// hole, only shrinking from the top the same way sbrk(negative) does.
#define SYS_munmap 25
// Ends the whole process, same as SYS_exit - poc-os has no notion of
// multiple threads within one process (so no distinction between
// "end this thread" and "end every thread"), but musl's _Exit()
// (musl/src/exit/_Exit.c) always calls SYS_exit_group unconditionally,
// never plain SYS_exit, so the kernel needs a name for it regardless.
// Points at the same sys_exit handler in kernel/syscall.c.
#define SYS_exit_group 26
// Returns the caller's thread ID; poc-os has no separate thread-id
// concept from pid (see SYS_exit_group above), so sys_set_tid_address
// (kernel/sysproc.c) just returns getpid() and otherwise ignores its
// argument (musl's __init_tp, musl/src/env/__init_tls.c, calls this on
// every process startup, threaded or not, to learn its tid).
#define SYS_set_tid_address 27
// Real futex semantics (blocking wait/wake on a userspace address) are
// unimplemented - poc-os has no threads to block one of yet. Given a
// number purely so musl's uncontended lock/unlock fast paths (which
// musl's own atomics resolve without ever reaching the syscall, single-
// threaded or not - see musl/src/internal/pthread_impl.h's __wake/
// __futexwait) compile; kernel/sysproc.c's sys_futex is a stub that
// always reports success without actually blocking or waking anything.
// Fine for single-threaded programs; will need a real implementation
// before pthread_create()-based multithreading works.
#define SYS_futex 28
// Not implemented at all (no dispatch table entry - see
// kernel/syscall.c) - genuinely unreachable for poc-os's musl port
// today. musl's __libc_start_main (musl/src/env/__libc_start_main.c)
// only calls SYS_ppoll on the "secure exec" path (AT_UID != AT_EUID or
// similar), and kernel/exec.c's execve() always sets those equal, so
// this exists purely so the SYS_ppoll macro musl's syscall.h derives
// from it has a value to compile against.
#define SYS_ppoll 29
// (addr): real Linux brk() semantics, not poc-os's own SYS_sbrk's -
// addr==0 queries the current break, otherwise it's the desired
// absolute break address, and the return value is always the
// resulting break (the old one, unchanged, if the request failed) -
// never -1, even on failure. musl's oldmalloc backend
// (musl/src/malloc/oldmalloc/malloc.c) calls this directly rather
// than going through any libc-level sbrk() wrapper. See
// kernel/sysproc.c's sys_brk().
#define SYS_brk 30
// Stub: always "succeeds" as a no-op. musl's oldmalloc backend
// (musl/src/malloc/oldmalloc/malloc.c) calls this as a pure
// optimization hint (advising the kernel it can reclaim pages backing
// a freed chunk) - fine to ignore correctness-wise, poc-os just never
// reclaims eagerly.
#define SYS_madvise 31
// Stub: always fails (returns -1, poc-os's universal syscall-failure
// value - see kernel/sysproc.c's sys_mremap()). Only reachable from
// realloc() on a chunk originally over MMAP_THRESHOLD (~114KB, see
// oldmalloc/malloc_impl.h); musl's realloc() falls back to alloc+copy+
// free on failure, so this is a real (if unoptimized) code path, not
// dead code the way SYS_ppoll is - a real mremap that can actually
// grow a mapping in place, rather than always forcing the fallback,
// is future work.
#define SYS_mremap 32
// (fd, iov, iovcnt): a real implementation (unlike madvise/mremap
// above) - musl's buffered stdio (musl/src/stdio/__stdio_write.c)
// calls this on every flush, to write its internal buffer and the
// caller's data in one syscall. Just loops filewrite() once per
// non-empty iovec (kernel/sysfile.c's sys_writev()); like sys_write,
// iov_base/iov_len are read as the low 32 bits of each 8-byte field
// (see fetchint's existing 64-bit-pointer caveat in sys_exec()).
#define SYS_writev 33
// Stub: always fails. musl's stdio (musl/src/stdio/__stdout_write.c)
// only calls this once, on the first write to a FILE, with
// TIOCGWINSZ, to decide whether to line-buffer (a terminal) or fully
// buffer (anything else) - always failing just means every stream
// looks like a non-terminal to musl, i.e. fully buffered. poc-os has
// no ioctl of any kind, terminal or otherwise, yet.
#define SYS_ioctl 34
// (fd, offset, whence): a real implementation, on struct file's
// existing off field (kernel/sysfile.c's sys_lseek()) - whence is
// SEEK_SET(0)/SEEK_CUR(1)/SEEK_END(2), the same values musl's
// <unistd.h> (and Linux) use, so no translation is needed. Only valid
// on FD_INODE files, matching real lseek's ESPIPE-on-a-pipe behavior.
#define SYS_lseek 35
// (addr, len, prot): changes the PTE_W bit on every page in an
// already-mapped [addr, addr+len) range within [0, proc->sz) to match
// prot's PROT_WRITE bit. poc-os's page tables track no NX-style
// execute-disable bit at all (every present page is executable), so
// PROT_EXEC/PROT_READ/PROT_NONE are accepted but otherwise ignored -
// see kernel/sysproc.c's sys_mprotect(). musl's dynamic linker calls
// this once per segment after relocation to drop write permission on
// anything that isn't meant to stay writable (e.g. a .so's .text/
// .rodata).
#define SYS_mprotect 36
// (fd, buf, count, offset): like SYS_read, but reads from a given
// offset without touching (or being affected by) the file's own
// read/write cursor - real pread() semantics. Only musl/ldso/
// dynlink.c's map_library() needs this (to read a shared object's
// program headers without disturbing the fd it's about to mmap from)
// - see kernel/sysfile.c's sys_pread().
#define SYS_pread 37
// (fd, cmd, arg): stub - always "succeeds" as a no-op. musl's open()
// (musl/src/fcntl/open.c) calls this once, with F_SETFD/FD_CLOEXEC,
// whenever O_CLOEXEC is requested; poc-os has no exec-time fd-closing
// behavior of any kind yet (every fd survives exec()), so a real
// implementation would have nothing to do anyway - see
// kernel/sysfile.c's sys_fcntl().
#define SYS_fcntl 38
// (fd, iov, iovcnt): the read-side mirror of SYS_writev above - musl's
// buffered stdio (musl/src/stdio/__stdio_read.c) calls this on every
// underlying fill, reading straight into both the caller's buffer and
// the FILE's own internal one in a single syscall the same way
// __stdio_write's flush does. Just loops fileread() once per non-empty
// iovec (kernel/sysfile.c's sys_readv()), stopping at the first short
// read exactly like a real readv() would; same iov_base/iov_len-as-
// low-32-bits caveat as sys_writev().
#define SYS_readv 39
// (fd, buf, count): real Linux getdents64 ABI - musl's readdir()
// (musl/src/dirent/readdir.c) issues this raw syscall directly (not
// through any C-level getdents() wrapper poc-os could override in
// userspace), so real directory listing needs an actual kernel
// implementation - see kernel/sysfile.c's sys_getdents() for the
// translation from poc-os's own on-disk directory format.
#define SYS_getdents 40
// (fd): see kernel/sysfile.c's sys_fchdir() - the same real chdir()
// backing logic, starting from an already-open directory fd instead
// of a fresh path lookup.
#define SYS_fchdir 41
// (fd, length): needed for cp/mv (coreutils/src/copy.c calls a real
// ftruncate() to record a sparse-copy's final length and, for mv's
// cross-device rename fallback, to fix up a partial write) - see
// kernel/sysfile.c's sys_ftruncate() and kernel/fs.c's itruncto().
#define SYS_ftruncate 42
// (old, new): a real rename(2) - see kernel/sysfile.c's sys_rename()
// for why moving a directory needs this rather than the link()+
// unlink() emulation coreutils_shims.c's rename() used to provide
// (still musl's own real src/stdio/rename.c, not a shim, now that
// this exists - see its own "#if defined(SYS_rename)" branch).
#define SYS_rename 43

// Multi-user support (uid/gid/mode + permission enforcement - see
// include/proc.h's identity fields, kernel/fs.c's permcheck(),
// include/fs.h's struct dinode). poc-os's own syscall ABI, like every
// number above - not Linux's, so these don't need to match any real
// SYS_getuid/SYS_setuid/etc numbering.
#define SYS_getuid   44
#define SYS_geteuid  45
#define SYS_getgid   46
#define SYS_getegid  47
#define SYS_setuid   48
#define SYS_seteuid  49
#define SYS_setgid   50
#define SYS_setegid  51
#define SYS_chmod    52
#define SYS_fchmod   53
#define SYS_chown    54
#define SYS_fchown   55
#define SYS_umask    56

// Wayland IPC primitives (GUI roadmap phase 3): AF_UNIX stream sockets
// with SCM_RIGHTS fd-passing, POSIX-ish shared memory, and epoll - see
// include/socket.h, include/shm.h, kernel/sysnet.c, kernel/epoll.c.
// poc-os's own syscall ABI, not Linux's - numbers are free to assign,
// they just need to match what musl/tools/gen-poc-syscall.sh derives
// __NR_x from (musl's own socket()/bind()/.../epoll_*() wrappers call
// these directly, confirmed by reading musl/src/network/*.c and
// musl/src/linux/epoll.c - no SYS_socketcall multiplexer is defined
// for this arch, so each socket op needs its own direct syscall).
#define SYS_socket   57
#define SYS_bind     58
#define SYS_listen   59
#define SYS_accept   60
#define SYS_connect  61
#define SYS_sendmsg  62
#define SYS_recvmsg  63
#define SYS_socketpair 64
// Not a real Linux/musl syscall name - shm_open() is a plain open() in
// musl with no dedicated syscall (see include/shm.h's own comment for
// why this port doesn't hook that path yet). Called directly via a raw
// syscall(SYS_shm_create, size) the same way bash/poc/fbtest.c already
// calls syscall(SYS_mknod, ...) raw.
#define SYS_shm_create 65
#define SYS_epoll_create1 66
#define SYS_epoll_ctl 67
#define SYS_epoll_pwait 68
// (struct rtcdate *r): reads the real-time clock into *r via
// kernel/lapic.c's cmostime() (CMOS/RTC hardware) - that function has
// existed since the original xv6 skeleton but was never wired to a
// syscall until the desktop panel's clock widget (GUI roadmap
// ToaruOS-style rewrite) needed real wall-clock time. Not a real
// Linux/musl syscall; called directly via syscall(SYS_date, &r), same
// convention as SYS_shm_create.
#define SYS_date 69
// (int fd): non-blocking peek at whether a send on this AF_UNIX socket
// fd would fit in its queue without blocking - kernel/socket.c's own
// sockwritable(), the same check kernel/epoll.c's EPOLLOUT support
// already calls internally, just exposed directly for a caller (gui/
// compositor.c) that wants a yes/no answer for exactly one fd right
// before deciding whether to send, not a whole epoll_wait() round
// trip. Returns 1 if a send would fit (or the peer's gone - the send
// would fail immediately rather than block either way), 0 if the
// queue is currently full. Not a real Linux/musl syscall; called
// directly via syscall(SYS_sock_writable, fd), same convention as
// SYS_shm_create/SYS_date.
#define SYS_sock_writable 70

// arch_prctl "code" values. Kept the same numeric value Linux (and so
// musl's __set_thread_area.s) uses for ARCH_SET_FS, purely so a value
// musl already hardcodes doesn't also need to change - poc-os's
// sys_arch_prctl (kernel/sysproc.c) doesn't implement any of the
// others (ARCH_GET_FS, ARCH_SET_GS, ...).
#define ARCH_SET_FS 0x1002
