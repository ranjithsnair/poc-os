/* int 0x80 syscall gate -- PoC-OS's own ABI, not Linux's. */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* Number in rax, args in rdi/rsi/rdx; return value written back into
 * rax (the syscall stub preserves it across the trip back to ring3).
 * Every syscall below fits this 3-argument convention -- r8-r11 are
 * already saved in struct registers (isr.h) but nothing here needs a
 * 4th argument yet. */
#define SYS_WRITE_CHAR     0  /* rdi = character to print (by value, not a pointer) */
#define SYS_EXIT           1  /* rdi = exit status (informational only -- no wait() to report it to yet) */
#define SYS_READ           2  /* rdi = fd, rsi = user buf ptr, rdx = len -> rax = bytes read, or -1 */
#define SYS_WRITE          3  /* rdi = fd, rsi = user buf ptr, rdx = len -> rax = bytes written, or -1 */
#define SYS_OPEN           4  /* rdi = user path ptr, rsi = flags (O_* below) -> rax = fd, or -1.
                                * Relative paths (no leading '/') resolve against the calling
                                * process's cwd -- see vfs.c. */
#define SYS_CLOSE          5  /* rdi = fd -> rax = 0, or -1 */
#define SYS_LSEEK          6  /* rdi = fd, rsi = offset (signed), rdx = whence -> rax = new offset, or -1 */
#define SYS_FSTAT          7  /* rdi = fd, rsi = user struct poc_stat* -> rax = 0, or -1 */
#define SYS_ANON_ALLOCATE  8  /* rdi = size in bytes -> rax = base VA, or 0 */
#define SYS_ANON_FREE      9  /* rdi = VA (must exactly match a live SYS_ANON_ALLOCATE's return
                                * value), rsi = size -> rax = 0, or -1 if no such live allocation
                                * exists. Unmaps and frees the physical frames for real (see
                                * process_anon_free()) -- only the virtual address range itself
                                * is never reclaimed. */
#define SYS_SET_FS_BASE    10 /* rdi = value -> rax = 0 */
#define SYS_IOCTL          11 /* rdi = fd (ignored -- there's exactly one console/tty in this
                                * kernel), rsi = request (TCGETS/TCSETS/TIOCGPGRP/TIOCSPGRP
                                * below), rdx = request-specific value or user pointer -> rax = 0,
                                * or -1 on failure; an unrecognized request still just succeeds,
                                * so a libc's isatty()-style probing doesn't fault. */
#define SYS_GETPID         12 /* -> rax = pid */
#define SYS_SIGACTION      13 /* rdi=signum, rsi=handler (0=SIG_DFL, 1=SIG_IGN, else=handler
                                * address), rdx=restorer (an *executable* address -- part of the
                                * caller's own image, e.g. a libc-provided trampoline -- that the
                                * kernel points a caught signal's implicit return address at, so
                                * the handler's `ret` lands there instead of on the stack, which
                                * this kernel maps NX; the restorer's only job is to invoke
                                * SYS_SIGRETURN) -> rax = the previous handler value, or -1 on an
                                * invalid signum. */
#define SYS_SIGPROCMASK    14 /* rdi=how (0=SIG_BLOCK, 1=SIG_UNBLOCK, 2=SIG_SETMASK), rsi=mask
                                * value -> rax = the *previous* blocked-signal mask, so the
                                * caller can compute SIG_BLOCK/SIG_UNBLOCK's OR/AND-NOT against
                                * it itself -- this syscall only ever does SIG_SETMASK internally. */
#define SYS_GETCWD         15 /* rdi = user buf ptr, rsi = size -> rax = length written, or -1 */
#define SYS_CHDIR          16 /* rdi = user path ptr -> rax = 0, or -1 */
#define SYS_CLOCK_GET      17 /* -> rax = PIT tick count */
#define SYS_FORK           18 /* -> rax = child's pid in the parent, 0 in the child, or 0 in the
                                * parent if fork failed -- a failed fork never produces a child to
                                * be confused with a real "I'm the child" 0, so this is unambiguous
                                * despite reusing the same value. */
#define SYS_EXECVE         19 /* rdi = user path ptr, rsi = user argv (char*[], NULL-terminated,
                                * <=15 entries), rdx = user envp (same shape) -> rax = -1 on
                                * failure (bad path/ELF/too many args); never returns on success,
                                * since regs are overwritten in place to enter the new image. */
#define SYS_WAITPID        20 /* rdi = pid (-1 = any child), rsi = user int* for exit status (0 =
                                * don't care) -> rax = reaped child's pid, 0 if the caller has a
                                * matching child but none has exited yet (poll again, same
                                * never-block convention as SYS_READ), or -1 if no such child
                                * exists at all. */
#define SYS_PIPE           21 /* rdi = user int[2]* -> filled with [read_fd, write_fd] -> rax = 0,
                                * or -1 on failure. */
#define SYS_DUP2           22 /* rdi = oldfd, rsi = newfd -> rax = newfd, or -1 on failure. */
#define SYS_SIGRETURN      23 /* rdi = user ptr to a full struct registers (isr.h), previously
                                * saved onto the stack by the kernel when it delivered a caught
                                * signal -- see SYS_SIGACTION's `restorer` doc comment. Overwrites
                                * the entire trapped context (including rax) with what's pointed
                                * to, resuming exactly where the signal interrupted execution;
                                * never "returns" a value in the ordinary sense. */
#define SYS_KILL           24 /* rdi = target pid, rsi = signum -> rax = 0, or -1 (bad pid/signum). */
#define SYS_MKDIR          25 /* rdi = user path ptr (relative paths resolve against cwd, same as
                                * SYS_OPEN) -> rax = 0, or -1 (parent doesn't exist, name already
                                * exists, or name isn't 8.3-compatible -- see fat32.h). */
#define SYS_GETPPID        26 /* -> rax = parent pid, or 0 if boot-spawned (no parent) */

/* Signal numbers -- PoC-OS's own numbering (matches Linux's values, which
 * costs nothing and means a libc sysdeps layer's <bits/signal.h> can reuse
 * familiar constants), not a claim of ABI compatibility with anything. */
#define SIGINT  2
#define SIGKILL 9
#define SIGTERM 15
#define SIGCHLD 17
#define POC_NSIG 32

/* Sentinel handler values for SYS_SIGACTION's `handler`/return value --
 * deliberately the same as every libc's SIG_DFL/SIG_IGN (0 and 1, cast to
 * a function pointer), so a sysdeps layer can pass its own SIG_DFL/
 * SIG_IGN straight through without translation. */
#define POC_SIG_DFL 0
#define POC_SIG_IGN 1

/* SYS_IOCTL requests -- console.c is the single global TTY these all
 * address (this kernel has exactly one console, so there's no fd-
 * specific dispatch to do). */
#define TCGETS    1 /* rdx = user uint32_t* -> filled with the current c_lflag bits */
#define TCSETS    2 /* rdx = user uint32_t* -> new c_lflag bits to apply */
#define TIOCGPGRP 3 /* rdx = user uint64_t* -> filled with the current foreground pid */
#define TIOCSPGRP 4 /* rdx = the new foreground pid, passed by value (not a pointer -- it's a
                     * single scalar, and every other ioctl request here that only moves one
                     * value keeps the same by-value convention SYS_SIGPROCMASK's mask uses) */

/* c_lflag bits for TCGETS/TCSETS -- this kernel's own bit positions (a
 * sysdeps <bits/termios.h> maps POSIX's ICANON/ECHO onto these, exactly
 * as real termios bit values are already OS-specific, not POSIX-fixed). */
#define POC_ICANON (1u << 0)
#define POC_ECHO   (1u << 1)

/* SYS_READ on a pipe read end (SYS_WRITE on a pipe write end) can return
 * two additional negative values beyond the plain "-1 = error" every
 * other fd kind uses, since a pipe -- unlike a console or a finite
 * tarfs-backed file -- genuinely has three distinct outcomes to report
 * and no way to block until one becomes true (same reason SYS_READ never
 * blocks at all -- see syscall.c): data transferred (>0), truly at
 * end-of-file/broken-pipe (0 on read once all writers are gone; -1 on
 * write once all readers are gone), or merely empty/full right now with
 * the other end still open, which is neither of those and must not be
 * mistaken for one -- SYS_PIPE_AGAIN below. */
#define SYS_PIPE_AGAIN     (-2)

/* SYS_OPEN flags -- deliberately the same bit values as Linux's, same
 * reasoning as the signal numbers above: costs nothing, and means a
 * sysdeps <bits/fcntl.h> can reuse familiar constants. Only these five
 * bits are understood; anything else is silently ignored (e.g. there's
 * no O_NONBLOCK to speak of, since nothing here ever blocks anyway). */
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

/* lseek whence values -- deliberately the same as POSIX's SEEK_SET/CUR/
 * END (0/1/2), so a real libc's sysdeps layer needs no translation. */
#define SYS_SEEK_SET 0
#define SYS_SEEK_CUR 1
#define SYS_SEEK_END 2

/* Minimal stat structure for SYS_FSTAT -- not POSIX's struct stat (this
 * kernel has no inode numbers, timestamps, link counts, etc. to report),
 * just enough for a libc sysdeps layer to answer isatty()/fstat()-driven
 * buffering decisions. st_mode's format bits follow POSIX (S_IFREG =
 * 0100000, S_IFCHR = 0020000) so a real libc's existing S_ISREG/S_ISCHR
 * macros work unmodified against it. */
struct poc_stat {
    uint64_t st_size;
    uint32_t st_mode;
};

/* Installs the vector-0x80 IDT gate. Called from idt_init(). */
void syscall_install(void);

#endif
