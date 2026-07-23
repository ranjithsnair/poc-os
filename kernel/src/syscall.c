/*
 * Syscall dispatch for the int 0x80 gate (asm_stubs.S: syscall_stub /
 * syscall_common_stub). SYS_WRITE_CHAR still takes its argument by
 * value (a holdover from before usercopy.c existed); every syscall added
 * since goes through copy_from_user()/copy_to_user() to safely touch a
 * caller-supplied buffer.
 *
 * SYS_READ deliberately never blocks (see process_fd_read()/
 * console_read_nonblock()): int 0x80 is a DPL3 *interrupt* gate
 * (syscall_install() below), which clears IF on entry, so a naive
 * busy-wait inside a blocking read here would deadlock against the very
 * keyboard IRQ it's waiting on. Retry-on-EOF-ish-zero belongs in
 * userspace (a real libc's read() wrapper), not in the kernel.
 */
#include <stdint.h>
#include "syscall.h"
#include "idt.h"
#include "isr.h"
#include "process.h"
#include "usercopy.h"
#include "serial.h"
#include "heap.h"
#include "pit.h"

extern void syscall_stub(void);

#define SYS_RW_MAX_CHUNK 65536
#define SYS_PATH_MAX 256

static void sys_write_char(struct registers *regs) {
    serial_putc((char)regs->rdi);
    regs->rax = 0;
}

/* Terminates the calling process and reschedules in its place -- *regs
 * gets overwritten with whatever process.c picks next, so the caller's
 * POP_ALL+iretq (syscall_common_stub) never resumes the exited one. */
static void sys_exit(struct registers *regs) {
    serial_print("PoC-OS: process pid=");
    serial_print_dec(process_current_pid());
    serial_print(" exited with status ");
    serial_print_dec(regs->rdi);
    serial_print(".\n");
    process_exit_current(regs);
}

/* Copies a NUL-terminated string out of user memory one byte at a time,
 * validating each byte individually rather than the whole range up
 * front -- a fixed-size copy_from_user() of, say, 256 bytes would
 * spuriously fail for a short string sitting near the end of a mapped
 * region even though every byte the caller actually cares about is
 * perfectly readable. Returns 1 on success (kbuf holds a NUL-terminated
 * string), 0 if the string doesn't fit in max_len or hits an unmapped
 * byte first. */
static int copy_user_cstr(uint64_t pml4, uint64_t user_ptr, char *kbuf, uint64_t max_len) {
    for (uint64_t i = 0; i < max_len; i++) {
        uint8_t byte;
        if (!copy_from_user(pml4, &byte, user_ptr + i, 1)) {
            return 0;
        }
        kbuf[i] = (char)byte;
        if (byte == 0) {
            return 1;
        }
    }
    return 0;
}

static void sys_read(struct registers *regs) {
    int fd = (int)regs->rdi;
    uint64_t user_buf = regs->rsi;
    uint64_t len = regs->rdx;
    if (len > SYS_RW_MAX_CHUNK) {
        len = SYS_RW_MAX_CHUNK;
    }

    uint8_t *kbuf = (uint8_t *)kmalloc(len ? len : 1);
    if (kbuf == NULL) {
        regs->rax = (uint64_t)-1;
        return;
    }

    int64_t n = process_fd_read(fd, kbuf, len);
    if (n > 0 && !copy_to_user(process_current_pml4(), user_buf, kbuf, (uint64_t)n)) {
        n = -1;
    }
    kfree(kbuf);
    regs->rax = (uint64_t)n;
}

static void sys_write(struct registers *regs) {
    int fd = (int)regs->rdi;
    uint64_t user_buf = regs->rsi;
    uint64_t len = regs->rdx;
    if (len > SYS_RW_MAX_CHUNK) {
        len = SYS_RW_MAX_CHUNK;
    }

    uint8_t *kbuf = (uint8_t *)kmalloc(len ? len : 1);
    if (kbuf == NULL) {
        regs->rax = (uint64_t)-1;
        return;
    }
    if (!copy_from_user(process_current_pml4(), kbuf, user_buf, len)) {
        kfree(kbuf);
        regs->rax = (uint64_t)-1;
        return;
    }

    int64_t n = process_fd_write(fd, kbuf, len);
    kfree(kbuf);
    regs->rax = (uint64_t)n;
}

static void sys_open(struct registers *regs) {
    char path[SYS_PATH_MAX];
    if (!copy_user_cstr(process_current_pml4(), regs->rdi, path, sizeof(path))) {
        regs->rax = (uint64_t)-1;
        return;
    }
    regs->rax = (uint64_t)process_fd_open(path);
}

static void sys_close(struct registers *regs) {
    regs->rax = (uint64_t)process_fd_close((int)regs->rdi);
}

static void sys_lseek(struct registers *regs) {
    regs->rax = (uint64_t)process_fd_lseek((int)regs->rdi, (int64_t)regs->rsi, (int)regs->rdx);
}

static void sys_fstat(struct registers *regs) {
    uint64_t size;
    uint32_t mode;
    if (process_fd_fstat((int)regs->rdi, &size, &mode) != 0) {
        regs->rax = (uint64_t)-1;
        return;
    }
    struct poc_stat st = { .st_size = size, .st_mode = mode };
    if (!copy_to_user(process_current_pml4(), regs->rsi, &st, sizeof(st))) {
        regs->rax = (uint64_t)-1;
        return;
    }
    regs->rax = 0;
}

static void sys_anon_allocate(struct registers *regs) {
    regs->rax = process_anon_allocate(regs->rdi);
}

static void sys_anon_free(struct registers *regs) {
    /* No-op -- see process_anon_allocate()'s doc comment. */
    regs->rax = 0;
}

static void sys_set_fs_base(struct registers *regs) {
    process_set_fs_base(regs->rdi);
    regs->rax = 0;
}

static void sys_ioctl(struct registers *regs) {
    /* Always "succeeds" -- there's no real termios/winsize to report,
     * this exists only so a libc's isatty()-style probing doesn't fault
     * on an unimplemented syscall. */
    regs->rax = 0;
}

static void sys_getpid(struct registers *regs) {
    regs->rax = process_current_pid();
}

static void sys_sigaction(struct registers *regs) {
    /* Always succeeds; no signal is ever actually delivered. */
    regs->rax = 0;
}

static void sys_sigprocmask(struct registers *regs) {
    regs->rax = 0;
}

static void sys_getcwd(struct registers *regs) {
    char kbuf[64];
    int n = process_getcwd(kbuf, sizeof(kbuf));
    if (n < 0 || (uint64_t)(n + 1) > regs->rsi) {
        regs->rax = (uint64_t)-1;
        return;
    }
    if (!copy_to_user(process_current_pml4(), regs->rdi, kbuf, (uint64_t)(n + 1))) {
        regs->rax = (uint64_t)-1;
        return;
    }
    regs->rax = (uint64_t)n;
}

static void sys_chdir(struct registers *regs) {
    char path[SYS_PATH_MAX];
    if (!copy_user_cstr(process_current_pml4(), regs->rdi, path, sizeof(path))) {
        regs->rax = (uint64_t)-1;
        return;
    }
    regs->rax = (uint64_t)process_chdir(path);
}

static void sys_clock_get(struct registers *regs) {
    regs->rax = pit_get_ticks();
}

/* Called by syscall_common_stub. */
void syscall_dispatch(struct registers *regs) {
    switch (regs->rax) {
        case SYS_WRITE_CHAR:    sys_write_char(regs); break;
        case SYS_EXIT:          sys_exit(regs); break;
        case SYS_READ:          sys_read(regs); break;
        case SYS_WRITE:         sys_write(regs); break;
        case SYS_OPEN:          sys_open(regs); break;
        case SYS_CLOSE:         sys_close(regs); break;
        case SYS_LSEEK:         sys_lseek(regs); break;
        case SYS_FSTAT:         sys_fstat(regs); break;
        case SYS_ANON_ALLOCATE: sys_anon_allocate(regs); break;
        case SYS_ANON_FREE:     sys_anon_free(regs); break;
        case SYS_SET_FS_BASE:   sys_set_fs_base(regs); break;
        case SYS_IOCTL:         sys_ioctl(regs); break;
        case SYS_GETPID:        sys_getpid(regs); break;
        case SYS_SIGACTION:     sys_sigaction(regs); break;
        case SYS_SIGPROCMASK:   sys_sigprocmask(regs); break;
        case SYS_GETCWD:        sys_getcwd(regs); break;
        case SYS_CHDIR:         sys_chdir(regs); break;
        case SYS_CLOCK_GET:     sys_clock_get(regs); break;
        default:
            serial_print("PoC-OS: unknown syscall number, ignoring.\n");
            regs->rax = (uint64_t)-1;
            break;
    }
}

void syscall_install(void) {
    /* 0xEE = present, DPL3, 64-bit interrupt gate -- DPL3 is what lets
     * ring3 code execute `int 0x80` at all; every other gate in this
     * kernel is DPL0 and would fault if user code tried it. */
    idt_set_gate(0x80, syscall_stub, 0, 0xEE);
}
