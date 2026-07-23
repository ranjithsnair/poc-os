/*
 * Preemptive round-robin scheduler for ring3 processes. There's no
 * filesystem yet, so process_create() takes an in-memory code blob
 * rather than loading an ELF from disk -- that's the natural extension
 * point once a filesystem exists (an exec() syscall would read the file
 * into a buffer and hand it to the same primitive this uses internally).
 */
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "isr.h"

/* Creates a new ring3 process: its own address space, a code page
 * holding a copy of `code[0..code_size)` mapped executable+user at
 * `entry_virt`, and a stack page mapped immediately after it. The code
 * must be position-independent in the sense user_test.S documents (no
 * absolute-address references), since it's copied to a fresh physical
 * frame and mapped somewhere new. Returns the new PID, or 0 on failure
 * (out of memory, or no free process slots). */
uint64_t process_create(const uint8_t *code, uint64_t code_size, uint64_t entry_virt);

/* Creates a new ring3 process from a real ELF64 image (data[0..size)),
 * via elf.c's loader, with the given argv/envp built onto its initial
 * stack (see elf_build_user_stack()). Returns the new PID, or 0 on
 * failure (bad ELF, out of memory, or no free process slots) -- the
 * partially-built address space is torn down before returning 0. */
uint64_t process_create_from_elf(const uint8_t *data, uint64_t size,
                                  int argc, const char *const argv[],
                                  int envc, const char *const envp[]);

/* Called from pit.c's tick callback: saves the interrupted context (if
 * it belonged to a running process), picks the next READY process
 * round-robin, and overwrites *regs with its saved context -- the
 * caller's existing POP_ALL+iretq then resumes that process instead. */
void scheduler_tick(struct registers *regs);

/* Called from syscall.c's SYS_EXIT handler: marks the currently running
 * process unused and immediately reschedules, the same way, so the
 * caller's POP_ALL+iretq never resumes the now-dead process. Tears down
 * the exiting process's address space and kernel stack (no more of the
 * old deliberate leak -- see vmm_destroy_address_space()). */
void process_exit_current(struct registers *regs);

/* Accessors syscall.c uses to implement the syscalls that operate on
 * "whoever is currently running" -- there is no other way to reach a
 * process's state from outside process.c, by design (struct process
 * itself stays private to this file). Each returns a harmless default
 * (0, or a fixed-value failure) if called with no process scheduled,
 * which cannot happen during real syscall dispatch (syscalls only ever
 * run because a process trapped into the kernel) but is defined
 * behavior rather than undefined for defensiveness's sake. */
uint64_t process_current_pid(void);
uint64_t process_current_pml4(void);

/* fd table operations, all scoped to the currently running process.
 * fd 0/1/2 are pre-opened as the console (stdin/stdout/stderr) by every
 * process_spawn(); process_fd_open() opens additional fds backed
 * directly by a tarfs file (there's no writable filesystem, so this is
 * the only kind of fd SYS_OPEN can ever hand back). */
int process_fd_open(const char *path);
int64_t process_fd_read(int fd, void *kbuf, uint64_t len);
int64_t process_fd_write(int fd, const void *kbuf, uint64_t len);
int process_fd_close(int fd);
int64_t process_fd_lseek(int fd, int64_t offset, int whence);
int process_fd_fstat(int fd, uint64_t *out_size, uint32_t *out_mode);

/* Bump-allocates `size` bytes (rounded up to whole pages) of freshly
 * mapped, zeroed, anonymous memory in the current process's address
 * space, starting at VMM_USER_ANON_BASE and growing upward on every
 * call -- this is the backing for mlibc's sys_anon_allocate hook (there
 * is no real virtual memory manager here, just a watermark that never
 * goes down: process_anon_free() is a deliberate no-op, consistent with
 * this kernel's existing per-process memory model). Returns the base
 * VA of the new region, or 0 on failure (out of memory) or if size is 0. */
uint64_t process_anon_allocate(uint64_t size);

/* Sets the current process's FS.base (used for thread-local storage by
 * a real libc) via wrmsr, and records it on the PCB so schedule() can
 * restore it every time this process is switched back in -- omitting
 * that restore would silently corrupt TLS/errno the moment a second
 * process runs, since FS.base is per-CPU-core state, not per-address-space. */
void process_set_fs_base(uint64_t value);

int process_getcwd(char *kbuf, uint64_t size);
int process_chdir(const char *kpath);

#endif
