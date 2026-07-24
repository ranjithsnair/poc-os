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

/* Called from syscall.c's SYS_EXIT handler: tears down the exiting
 * process's address space and kernel stack (via vmm_destroy_address_space())
 * and immediately reschedules, so the caller's POP_ALL+iretq never resumes
 * the now-dead process. If the process has a parent that might collect
 * its exit status (parent_pid != 0), the PCB slot itself becomes a zombie
 * -- kept around, unschedulable, until process_waitpid() reaps it; a
 * boot-spawned process with no parent to wait for it is freed immediately
 * instead, since nothing could ever reap it otherwise. */
void process_exit_current(struct registers *regs, int status);

/* Duplicates the calling process: a deep copy of its entire address space
 * (vmm_clone_address_space()), a shallow copy of its fd table (each fd
 * ends up with its own independent offset after fork -- real UNIX shares
 * the underlying open-file-description's offset across a fork, but that
 * needs an indirection this kernel's fd table doesn't have yet; good
 * enough for bring-up), and a copy of `*regs` (the trapped syscall
 * context) with rax forced to 0, becoming the child's saved context.
 * Returns the new child's pid to the parent (whose own regs->rax the
 * caller must still set), or 0 on failure -- never returns "as the
 * child", since the child only starts running on a later scheduler
 * tick, with its own already-0 rax. */
uint64_t process_fork(struct registers *regs);

/* Replaces the calling process's entire address space with a fresh ELF
 * image loaded from `path`, argv/envp built the same way
 * process_create_from_elf() builds them for a brand-new process. On
 * success, mutates *regs in place (rip/rsp to the new image's entry/
 * stack, general registers zeroed) and returns 1 -- the old address
 * space is destroyed only after CR3 has already moved to the new one, so
 * there's never a moment where a live CR3 points at freed frames. On
 * failure (bad path, bad ELF, out of memory), *regs is left completely
 * untouched and 0 is returned, matching execve()'s "failure leaves the
 * calling image running" contract. */
int process_execve(struct registers *regs, const char *path,
                    int argc, const char *const argv[],
                    int envc, const char *const envp[]);

/* Looks for a child of the calling process matching `target_pid` (-1 = any
 * child). If a matching child has already exited, reaps it (frees its PCB
 * slot for reuse) and returns its pid with *out_status set to what it
 * passed to SYS_EXIT. If a matching child exists but hasn't exited yet,
 * returns 0 (poll again -- this never blocks, for the same int-0x80-is-an-
 * interrupt-gate reason SYS_READ never blocks; see syscall.c). Returns -1
 * if the caller has no such child at all, alive or dead. */
int64_t process_waitpid(int64_t target_pid, int *out_status);

/* Accessors syscall.c uses to implement the syscalls that operate on
 * "whoever is currently running" -- there is no other way to reach a
 * process's state from outside process.c, by design (struct process
 * itself stays private to this file). Each returns a harmless default
 * (0, or a fixed-value failure) if called with no process scheduled,
 * which cannot happen during real syscall dispatch (syscalls only ever
 * run because a process trapped into the kernel) but is defined
 * behavior rather than undefined for defensiveness's sake. */
uint64_t process_current_pid(void);
uint64_t process_current_ppid(void);
uint64_t process_current_pml4(void);

/* fd table operations, all scoped to the currently running process.
 * fd 0/1/2 are pre-opened as the console (stdin/stdout/stderr) by every
 * process_spawn(); process_fd_open() opens additional fds via vfs.c
 * (resolved against the calling process's own cwd), applying `flags`
 * (SYS_OPEN's O_* bits, see syscall.h). Returns the new fd, or -1. */
int process_fd_open(const char *path, int flags);
int64_t process_fd_read(int fd, void *kbuf, uint64_t len);
int64_t process_fd_write(int fd, const void *kbuf, uint64_t len);
int process_fd_close(int fd);
int64_t process_fd_lseek(int fd, int64_t offset, int whence);
int process_fd_fstat(int fd, uint64_t *out_size, uint32_t *out_mode);

/* Creates a pipe: two new fds in the calling process, *out_read_fd and
 * *out_write_fd, sharing one in-kernel ring buffer. Returns 0, or -1 on
 * failure (out of memory or fewer than 2 free fd slots). */
int process_pipe_create(int *out_read_fd, int *out_write_fd);

/* Makes `newfd` an independent reference to whatever `oldfd` currently
 * refers to in the calling process (POSIX dup2() semantics: closes
 * whatever `newfd` used to be first). Returns newfd, or -1 if oldfd isn't
 * open or newfd is out of range. */
int process_fd_dup2(int oldfd, int newfd);

/* Bump-allocates `size` bytes (rounded up to whole pages) of freshly
 * mapped, zeroed, anonymous memory in the current process's address
 * space, starting at VMM_USER_ANON_BASE and growing upward on every
 * call -- this is the backing for mlibc's sys_vm_map/sys_anon_allocate
 * hook. The *virtual address space* backing this is still a watermark
 * that never goes down (VA space is effectively unlimited here, so
 * that's cheap), but the region is tracked precisely enough that
 * process_anon_free() can give the *physical* memory back for real --
 * see its doc comment. Returns the base VA of the new region, or 0 on
 * failure (out of memory, no free VMA-tracking slot, or size is 0). */
uint64_t process_anon_allocate(uint64_t size);

/* Unmaps and frees the physical frames backing a region exactly as
 * returned by a previous process_anon_allocate(size) call (base must
 * match exactly -- this isn't a general sub-range unmap). Returns 0 on
 * success, -1 if no such live allocation exists. */
int process_anon_free(uint64_t base, uint64_t size);

/* Sets the current process's FS.base (used for thread-local storage by
 * a real libc) via wrmsr, and records it on the PCB so schedule() can
 * restore it every time this process is switched back in -- omitting
 * that restore would silently corrupt TLS/errno the moment a second
 * process runs, since FS.base is per-CPU-core state, not per-address-space. */
void process_set_fs_base(uint64_t value);

int process_getcwd(char *kbuf, uint64_t size);
int process_chdir(const char *kpath);

/* Creates a directory at `kpath` (resolved against the calling process's
 * cwd the same way process_fd_open() resolves relative paths). Returns
 * 0 on success, -1 on failure (see vfs_mkdir()/fat32_mkdir()). */
int process_mkdir(const char *kpath);

/* Sets the calling process's disposition for `sig` to `handler`
 * (POC_SIG_DFL/POC_SIG_IGN/a real handler address) and records
 * `restorer` (see SYS_SIGACTION's doc comment) as the address any future
 * caught signal's trampoline frame should return through. Returns the
 * previous handler value, or -1 if `sig` is out of range. */
int64_t process_sigaction(int sig, uint64_t handler, uint64_t restorer);

/* Sets the calling process's blocked-signal mask according to `how`
 * (SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK semantics, applied against `mask`)
 * and returns the mask as it was *before* this call. */
uint64_t process_sigprocmask(int how, uint64_t mask);

/* Marks `sig` pending on the process with pid `target_pid` -- delivered
 * the next time that process is scheduled to run (see schedule() in
 * process.c), not synchronously. Returns 0, or -1 if no such process
 * exists or `sig` is out of range. */
int process_send_signal(uint64_t target_pid, int sig);

#endif
