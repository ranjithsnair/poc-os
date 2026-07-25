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
#include "console.h"

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
    process_exit_current(regs, (int)regs->rdi);
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
    if (fd == 0 && n != -2) {
        serial_print("DBG[t=");
        serial_print_dec(pit_get_ticks());
        serial_print("]: read(fd=0, len=");
        serial_print_dec(len);
        serial_print(") = ");
        serial_print_dec((uint64_t)n);
        serial_print("\n");
    }
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
    regs->rax = (uint64_t)process_fd_open(path, (int)regs->rsi);
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
    regs->rax = (uint64_t)process_anon_free(regs->rdi, regs->rsi);
}

static void sys_anon_allocate_fixed(struct registers *regs) {
    regs->rax = process_anon_allocate_fixed(regs->rdi, regs->rsi);
}

static void sys_mprotect(struct registers *regs) {
    regs->rax = (uint64_t)process_mprotect(regs->rdi, regs->rsi, regs->rdx);
}

static void sys_set_fs_base(struct registers *regs) {
    process_set_fs_base(regs->rdi);
    regs->rax = 0;
}

static void sys_ioctl(struct registers *regs) {
    uint64_t request = regs->rsi;
    uint64_t arg = regs->rdx;

    switch (request) {
        case TCGETS: {
            uint32_t lflag = console_get_lflag();
            serial_print("DBG: TCGETS -> lflag=0x");
            serial_print_dec(lflag);
            serial_print("\n");
            regs->rax = copy_to_user(process_current_pml4(), arg, &lflag, sizeof(lflag)) ? 0 : (uint64_t)-1;
            return;
        }
        case TCSETS: {
            uint32_t lflag;
            if (!copy_from_user(process_current_pml4(), &lflag, arg, sizeof(lflag))) {
                regs->rax = (uint64_t)-1;
                return;
            }
            serial_print("DBG: TCSETS lflag=0x");
            serial_print_dec(lflag);
            serial_print("\n");
            console_set_lflag(lflag);
            regs->rax = 0;
            return;
        }
        case TIOCGPGRP: {
            uint64_t pid = console_get_foreground_pid();
            regs->rax = copy_to_user(process_current_pml4(), arg, &pid, sizeof(pid)) ? 0 : (uint64_t)-1;
            return;
        }
        case TIOCSPGRP:
            console_set_foreground_pid(arg);
            regs->rax = 0;
            return;
        default:
            /* Always "succeeds" for anything else -- this exists only so
             * a libc's isatty()-style probing doesn't fault on a request
             * this kernel doesn't need to actually implement. */
            regs->rax = 0;
            return;
    }
}

static void sys_getpid(struct registers *regs) {
    regs->rax = process_current_pid();
}

static void sys_getppid(struct registers *regs) {
    regs->rax = process_current_ppid();
}

static void sys_sigaction(struct registers *regs) {
    regs->rax = (uint64_t)process_sigaction((int)regs->rdi, regs->rsi, regs->rdx);
}

static void sys_sigprocmask(struct registers *regs) {
    regs->rax = process_sigprocmask((int)regs->rdi, regs->rsi);
}

/* Overwrites the entire trapped context with what's saved at the user
 * pointer in rdi -- see SYS_SIGRETURN's doc comment. Deliberately does
 * NOT set regs->rax afterward: rax is part of the restored struct, and
 * whatever value it holds (the original syscall's return value, from
 * before the signal interrupted it) must survive untouched. */
static void sys_sigreturn(struct registers *regs) {
    copy_from_user(process_current_pml4(), regs, regs->rdi, sizeof(struct registers));
}

static void sys_kill(struct registers *regs) {
    regs->rax = (uint64_t)process_send_signal(regs->rdi, (int)regs->rsi);
}

static void sys_mkdir(struct registers *regs) {
    char path[SYS_PATH_MAX];
    if (!copy_user_cstr(process_current_pml4(), regs->rdi, path, sizeof(path))) {
        regs->rax = (uint64_t)-1;
        return;
    }
    regs->rax = (uint64_t)process_mkdir(path);
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

static void sys_getdents(struct registers *regs) {
    int fd = (int)regs->rdi;
    uint64_t user_buf = regs->rsi;
    uint64_t max_entries = regs->rdx;

    uint8_t *kbuf = (uint8_t *)kmalloc(max_entries * sizeof(struct poc_dirent));
    if (kbuf == NULL) {
        regs->rax = (uint64_t)-1;
        return;
    }
    int64_t n = process_fd_getdents(fd, kbuf, max_entries);
    if (n > 0 && !copy_to_user(process_current_pml4(), user_buf, kbuf, (uint64_t)n * sizeof(struct poc_dirent))) {
        n = -1;
    }
    kfree(kbuf);
    regs->rax = (uint64_t)n;
}

static void sys_unlink(struct registers *regs) {
    char path[SYS_PATH_MAX];
    if (!copy_user_cstr(process_current_pml4(), regs->rdi, path, sizeof(path))) {
        regs->rax = (uint64_t)-1;
        return;
    }
    regs->rax = (uint64_t)process_unlink(path);
}

static void sys_rmdir(struct registers *regs) {
    char path[SYS_PATH_MAX];
    if (!copy_user_cstr(process_current_pml4(), regs->rdi, path, sizeof(path))) {
        regs->rax = (uint64_t)-1;
        return;
    }
    regs->rax = (uint64_t)process_rmdir(path);
}

static void sys_rename(struct registers *regs) {
    char old_path[SYS_PATH_MAX];
    char new_path[SYS_PATH_MAX];
    if (!copy_user_cstr(process_current_pml4(), regs->rdi, old_path, sizeof(old_path)) ||
        !copy_user_cstr(process_current_pml4(), regs->rsi, new_path, sizeof(new_path))) {
        regs->rax = (uint64_t)-1;
        return;
    }
    regs->rax = (uint64_t)process_rename(old_path, new_path);
}

static void sys_fork(struct registers *regs) {
    regs->rax = process_fork(regs);
}

#define SYS_EXEC_MAX_ARGS 15

/* Reads a NULL-terminated array of user char* out of user memory into
 * kernel-owned string buffers, one pointer at a time -- mirrors
 * copy_user_cstr()'s "walk it, don't assume a fixed size" approach, since
 * the array's own length isn't known up front either. `bufs` must have
 * room for `max` strings of SYS_PATH_MAX bytes each; `ptrs[i]` is left
 * pointing at `bufs[i]` for each string found. Returns 1 on success
 * (*out_count holds how many), 0 on a bad pointer, a string that doesn't
 * fit, or more than `max` entries with no NULL terminator. */
static int copy_user_str_array(uint64_t pml4, uint64_t user_array,
                                char (*bufs)[SYS_PATH_MAX], const char *ptrs[],
                                int max, int *out_count) {
    for (int i = 0; i < max; i++) {
        uint64_t entry_ptr;
        if (!copy_from_user(pml4, &entry_ptr, user_array + (uint64_t)i * 8, 8)) {
            return 0;
        }
        if (entry_ptr == 0) {
            *out_count = i;
            return 1;
        }
        if (!copy_user_cstr(pml4, entry_ptr, bufs[i], SYS_PATH_MAX)) {
            return 0;
        }
        ptrs[i] = bufs[i];
    }
    return 0; /* max entries consumed without a NULL terminator */
}

static void sys_execve(struct registers *regs) {
    uint64_t pml4 = process_current_pml4();
    char path[SYS_PATH_MAX];
    if (!copy_user_cstr(pml4, regs->rdi, path, sizeof(path))) {
        regs->rax = (uint64_t)-1;
        return;
    }

    char (*argv_bufs)[SYS_PATH_MAX] = kmalloc(SYS_EXEC_MAX_ARGS * SYS_PATH_MAX);
    char (*envp_bufs)[SYS_PATH_MAX] = kmalloc(SYS_EXEC_MAX_ARGS * SYS_PATH_MAX);
    const char *argv_ptrs[SYS_EXEC_MAX_ARGS];
    const char *envp_ptrs[SYS_EXEC_MAX_ARGS];
    int argc = 0, envc = 0;

    int ok = (argv_bufs != NULL && envp_bufs != NULL &&
              copy_user_str_array(pml4, regs->rsi, argv_bufs, argv_ptrs, SYS_EXEC_MAX_ARGS, &argc) &&
              copy_user_str_array(pml4, regs->rdx, envp_bufs, envp_ptrs, SYS_EXEC_MAX_ARGS, &envc));

    if (ok) {
        ok = process_execve(regs, path, argc, argv_ptrs, envc, envp_ptrs);
    }

    if (argv_bufs != NULL) kfree(argv_bufs);
    if (envp_bufs != NULL) kfree(envp_bufs);

    if (!ok) {
        regs->rax = (uint64_t)-1;
    }
    /* On success process_execve() already overwrote rip/rsp/rax (and
     * every other general register) for the new image -- nothing to set
     * here without clobbering it. */
}

static void sys_pipe(struct registers *regs) {
    int fds[2];
    if (process_pipe_create(&fds[0], &fds[1]) != 0) {
        regs->rax = (uint64_t)-1;
        return;
    }
    if (!copy_to_user(process_current_pml4(), regs->rdi, fds, sizeof(fds))) {
        /* Roll back: the caller never learned these fd numbers, so leaking
         * them would be an unrecoverable pipe the process can never
         * close. */
        process_fd_close(fds[0]);
        process_fd_close(fds[1]);
        regs->rax = (uint64_t)-1;
        return;
    }
    regs->rax = 0;
}

static void sys_dup2(struct registers *regs) {
    regs->rax = (uint64_t)process_fd_dup2((int)regs->rdi, (int)regs->rsi);
}

static void sys_waitpid(struct registers *regs) {
    int status = 0;
    int64_t result = process_waitpid((int64_t)regs->rdi, &status);
    if (result > 0 && regs->rsi != 0) {
        if (!copy_to_user(process_current_pml4(), regs->rsi, &status, sizeof(status))) {
            regs->rax = (uint64_t)-1;
            return;
        }
    }
    regs->rax = (uint64_t)result;
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
        case SYS_FORK:          sys_fork(regs); break;
        case SYS_EXECVE:        sys_execve(regs); break;
        case SYS_WAITPID:       sys_waitpid(regs); break;
        case SYS_PIPE:          sys_pipe(regs); break;
        case SYS_DUP2:          sys_dup2(regs); break;
        case SYS_SIGRETURN:     sys_sigreturn(regs); break;
        case SYS_KILL:          sys_kill(regs); break;
        case SYS_MKDIR:         sys_mkdir(regs); break;
        case SYS_GETPPID:       sys_getppid(regs); break;
        case SYS_MPROTECT:      sys_mprotect(regs); break;
        case SYS_ANON_ALLOCATE_FIXED: sys_anon_allocate_fixed(regs); break;
        case SYS_GETDENTS:      sys_getdents(regs); break;
        case SYS_UNLINK:        sys_unlink(regs); break;
        case SYS_RMDIR:         sys_rmdir(regs); break;
        case SYS_RENAME:        sys_rename(regs); break;
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
