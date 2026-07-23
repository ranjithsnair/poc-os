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
#define SYS_OPEN           4  /* rdi = user path ptr, rsi = flags (ignored, read-only fs) -> rax = fd, or -1 */
#define SYS_CLOSE          5  /* rdi = fd -> rax = 0, or -1 */
#define SYS_LSEEK          6  /* rdi = fd, rsi = offset (signed), rdx = whence -> rax = new offset, or -1 */
#define SYS_FSTAT          7  /* rdi = fd, rsi = user struct poc_stat* -> rax = 0, or -1 */
#define SYS_ANON_ALLOCATE  8  /* rdi = size in bytes -> rax = base VA, or 0 */
#define SYS_ANON_FREE      9  /* rdi = VA, rsi = size -> rax = 0 (no-op: see process_anon_allocate()) */
#define SYS_SET_FS_BASE    10 /* rdi = value -> rax = 0 */
#define SYS_IOCTL          11 /* stub -> rax = 0 */
#define SYS_GETPID         12 /* -> rax = pid */
#define SYS_SIGACTION      13 /* stub, always succeeds -> rax = 0 */
#define SYS_SIGPROCMASK    14 /* stub, always succeeds -> rax = 0 */
#define SYS_GETCWD         15 /* rdi = user buf ptr, rsi = size -> rax = length written, or -1 */
#define SYS_CHDIR          16 /* rdi = user path ptr -> rax = 0, or -1 */
#define SYS_CLOCK_GET      17 /* -> rax = PIT tick count */

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
