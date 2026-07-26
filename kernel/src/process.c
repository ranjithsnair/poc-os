/*
 * A minimal preemptive round-robin scheduler, built entirely on
 * infrastructure the earlier milestones already established: every
 * process gets its own address space (vmm_create_address_space(),
 * sharing the kernel/HHDM/heap mappings every process needs) and its
 * own kernel stack (for TSS.RSP0).
 *
 * The trick that avoids needing a hand-written context-switch routine:
 * irq_common_stub (asm_stubs.S) already saves the interrupted context
 * into a `struct registers` sitting on the current kernel stack, calls
 * into C with a pointer to it, and does POP_ALL+iretq on whatever that
 * memory holds when the C handler returns. schedule() just overwrites
 * *regs with a *different* process's saved registers (and switches
 * CR3/TSS.RSP0/FS.base) before returning -- the existing POP_ALL+iretq
 * then resumes that process instead. The same trick works for a
 * process's very first run (nothing special needed beyond it being
 * READY) and for SYS_EXIT (process_exit_current() tears down the
 * address space and reschedules through the same path).
 */
#include <stddef.h>
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "gdt.h"
#include "heap.h"
#include "framebuffer.h"
#include "string.h"
#include "elf.h"
#include "console.h"
#include "syscall.h"
#include "usercopy.h"
#include "vfs.h"
#include "fpu.h"
#include "tarfs.h"

#define PROCESS_MAX 16
#define PROCESS_KERNEL_STACK_SIZE 16384
/* Matches sysdeps.cpp's Sysconf(_SC_OPEN_MAX) answer of 64 -- a real
 * shell's own redirection bookkeeping (saving the original fd 1 via
 * fcntl(1, F_DUPFD, 10)-style calls before a temporary dup2() redirect,
 * then restoring it afterward) routinely uses fd numbers well above the
 * 0/1/2 stdio range, so an fd table sized only for a handful of
 * explicitly-opened files (this used to be 8, back when the only
 * processes here were the hello_libc-style boot examples) silently
 * breaks that save/restore dance -- the save-to-fd-10 dup2() itself
 * would fail (newfd out of range), and the shell never notices before
 * trying to restore from it, leaving fd 1 wherever the last redirected
 * command's dup2() left it. */
#define PROCESS_FD_MAX 64
#define PROCESS_VMA_MAX 32

#define IA32_FS_BASE_MSR 0xC0000100u

static inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

enum process_state { PROCESS_UNUSED = 0, PROCESS_READY, PROCESS_RUNNING, PROCESS_ZOMBIE };

#define PIPE_BUF_SIZE 4096

/* A pipe is a shared ring buffer referenced by however many fds (across
 * however many processes -- dup2()/fork() both create new references to
 * the same one) currently hold either end open. It outlives any single
 * process_fd that points at it; process_fd_close()/process_fork() keep
 * `readers`/`writers` accurate, and process.c frees it once both drop to
 * 0. There's no blocking here (same reason SYS_READ never blocks -- see
 * syscall.c) -- process_fd_read()/process_fd_write() report "empty, but
 * still open" and "full" as distinct outcomes from real EOF/error so a
 * userspace read()/write() wrapper can tell them apart and poll instead
 * of misreading either as end-of-file. */
struct pipe {
    uint8_t buf[PIPE_BUF_SIZE];
    uint64_t read_pos;
    uint64_t count; /* bytes currently buffered */
    int readers;
    int writers;
};

/* console_kind: 0 = fat32-backed file/directory, 1 = stdin, 2 = stdout/
 * stderr, 3 = pipe read end, 4 = pipe write end, 5 = tarfs-backed
 * read-only file (initrd.tar, see tarfs.c -- e.g. mlibc's rtld resolving
 * /lib/libc.so at runtime, same as /busybox and its own PT_INTERP at
 * boot). `pipe` is only valid (non-NULL) for kinds 3/4; `fatfile`/
 * `writable` only for kind 0; `tarfs_data` only for kind 5 (always
 * read-only, so `writable` is left 0 there too). */
struct process_fd {
    int in_use;
    int console_kind;
    struct fat32_file fatfile;
    const uint8_t *tarfs_data;
    uint64_t size;
    uint64_t offset;
    uint32_t mode;
    struct pipe *pipe;
    int writable;
};

/* One anonymous mapping handed out by process_anon_allocate(), tracked
 * so process_anon_free() can find exactly which pages/frames to give
 * back -- see process_anon_allocate()'s doc comment in process.h for why
 * the address space behind it is still a bump allocator (VA space is
 * never reclaimed, only the physical frames are). */
struct vma {
    int in_use;
    uint64_t base;
    uint64_t size;
};

struct process {
    uint64_t pid;
    enum process_state state;
    uint64_t pml4_phys;
    uint8_t *kernel_stack;
    struct registers regs; /* saved context while not RUNNING */
    uint8_t fpu_state[512] __attribute__((aligned(16))); /* FXSAVE image, same "while not RUNNING" rule as regs */
    struct process_fd fds[PROCESS_FD_MAX];
    struct vma vmas[PROCESS_VMA_MAX];
    uint64_t next_anon_va;
    uint64_t fs_base;
    char cwd[64];
    uint64_t parent_pid; /* 0 = no parent (boot-spawned) -- see process_exit_current() */
    int exit_status;     /* only meaningful once state == PROCESS_ZOMBIE */
    uint64_t sig_pending;              /* bitmask, bit N = signal N is pending */
    uint64_t sig_blocked;              /* bitmask, set via SYS_SIGPROCMASK */
    uint64_t sig_handlers[POC_NSIG];   /* POC_SIG_DFL/POC_SIG_IGN/a handler address, per signal */
    uint64_t sig_restorer;             /* see SYS_SIGACTION's doc comment */
};

static struct process processes[PROCESS_MAX];
static int current_index = -1; /* slot *regs currently belongs to, or -1 */
static uint64_t next_pid = 1;

static struct process *current(void) {
    return (current_index == -1) ? NULL : &processes[current_index];
}

/* Shared tail of process_create_from_elf(): finds a free slot, allocates
 * the kernel stack, seeds the fd table with the console, and fills in
 * the initial saved-register state an iretq needs to enter ring3 at
 * `entry` with `stack_top` as %rsp. Does NOT touch the address space
 * itself -- callers must have already mapped everything the process
 * needs into `pml4` before calling this. */
static uint64_t process_spawn(uint64_t pml4, uint64_t entry, uint64_t stack_top) {
    int slot = -1;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROCESS_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        fb_print("PoC-OS: process_spawn: no free process slots.\n");
        return 0;
    }

    uint8_t *kernel_stack = (uint8_t *)kmalloc(PROCESS_KERNEL_STACK_SIZE);
    if (kernel_stack == NULL) {
        fb_print("PoC-OS: process_spawn: out of memory.\n");
        return 0;
    }

    struct process *p = &processes[slot];
    p->pid = next_pid++;
    p->pml4_phys = pml4;
    p->kernel_stack = kernel_stack;
    memcpy(p->fpu_state, fpu_default_state, sizeof(p->fpu_state));
    p->next_anon_va = VMM_USER_ANON_BASE;
    p->fs_base = 0;
    p->cwd[0] = '/';
    p->cwd[1] = '\0';
    p->parent_pid = 0;
    p->exit_status = 0;
    p->sig_pending = 0;
    p->sig_blocked = 0;
    for (int i = 0; i < POC_NSIG; i++) {
        p->sig_handlers[i] = POC_SIG_DFL;
    }
    p->sig_restorer = 0;
    for (int i = 0; i < PROCESS_VMA_MAX; i++) {
        p->vmas[i].in_use = 0;
    }

    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        p->fds[i].in_use = 0;
    }
    p->fds[0].in_use = 1;
    p->fds[0].console_kind = 1; /* stdin */
    p->fds[1].in_use = 1;
    p->fds[1].console_kind = 2; /* stdout */
    p->fds[2].in_use = 1;
    p->fds[2].console_kind = 2; /* stderr */

    for (size_t i = 0; i < sizeof(p->regs); i++) {
        ((uint8_t *)&p->regs)[i] = 0;
    }
    /* The saved context a first run needs is identical in shape to what
     * any iretq needs: an entry point, a ring3 code/data selector pair
     * (RPL=3), interrupts enabled, and a stack. General registers start
     * zeroed -- an ELF's crt0 reads argc/argv off the stack, not out of
     * registers, so this is correct for every process this kernel spawns. */
    p->regs.rip = entry;
    p->regs.cs = GDT_USER_CODE | 3;
    p->regs.rflags = 0x200; /* IF=1 */
    p->regs.rsp = stack_top;
    p->regs.ss = GDT_USER_DATA | 3;

    /* Flipped last, and last of all the fields the scheduler reads: a
     * tick landing between this line and process_spawn() returning
     * could pick this process up and run it, but by this point every
     * field it needs is already valid. */
    p->state = PROCESS_READY;

    fb_print("PoC-OS: process created.\n");
    return p->pid;
}

/* Fixed load-base constants for the two ELF images a PT_INTERP-linked
 * process needs mapped into one address space -- both chosen well clear
 * of each other, of VMM_USER_ANON_BASE/VMM_USER_STACK_TOP, and of the
 * interpreter's own internal DSO placement (mlibc's rtld bump-allocates
 * further shared libraries it loads starting at 0x41000000 -- see
 * mlibc/options/rtld/generic/linker.cpp's libraryBase). ELF_PIE_BASE
 * reuses the same address userland/linker.ld already links plain
 * ET_EXEC binaries at (0x400000): a PIE main executable's own segments
 * start at vaddr 0, so this is simply where they land once elf_load()
 * adds its load_base. */
#define ELF_PIE_BASE         0x0000000000400000ULL
#define ELF_INTERP_BASE      0x0000000010000000ULL
#define ELF_INTERP_PATH_MAX  192

/* Shared tail of process_create_from_elf() and process_execve(): loads
 * one program image -- the main executable at `data`/`size`, plus (if it
 * names one via PT_INTERP) the dynamic linker that resolves and
 * relocates it -- into a single freshly created address space, and
 * builds its initial stack. `cwd` resolves a relative PT_INTERP path the
 * same way vfs_open() resolves any other relative path (real PT_INTERP
 * paths are always absolute, e.g. "/lib/ld.so", but this doesn't assume
 * that). On success returns the new pml4 with out_entry/out_rsp filled
 * in (the rip/rsp process_spawn() or a SYS_EXECVE commit should use) --
 * the caller owns handing that off. Returns 0 on any failure, after
 * tearing down whatever partial address space was built. */
static uint64_t load_elf_program(const uint8_t *data, uint64_t size, const char *cwd,
                                  int argc, const char *const argv[],
                                  int envc, const char *const envp[],
                                  uint64_t *out_entry, uint64_t *out_rsp) {
    uint64_t pml4 = vmm_create_address_space();
    if (pml4 == 0) {
        return 0;
    }

    uint64_t main_base = elf_is_dyn(data, size) ? ELF_PIE_BASE : 0;
    struct elf_load_result main_elf;
    if (!elf_load(pml4, data, size, main_base, &main_elf)) {
        vmm_destroy_address_space(pml4);
        return 0;
    }

    uint64_t at_base = 0;
    uint64_t jump_entry = main_elf.entry;
    char interp_path[ELF_INTERP_PATH_MAX];
    if (elf_find_interp(data, size, interp_path, sizeof(interp_path))) {
        /* Tried first, same as spawn_boot_program()'s own /busybox
         * lookup in main.c: tarfs.c's initrd is already resident in RAM
         * (no leading '/' in its own file names), so a hit here needs no
         * kmalloc/free at all. Only if it's not there does this fall
         * back to the writable FAT32 disk. */
        const char *tarfs_name = (interp_path[0] == '/') ? interp_path + 1 : interp_path;
        uint64_t interp_size;
        const uint8_t *interp_data = tarfs_read(tarfs_name, &interp_size);
        int owns_interp_data = 0;

        if (interp_data == NULL) {
            struct fat32_file interp_file;
            if (!vfs_open(cwd, interp_path, O_RDONLY, &interp_file) || interp_file.is_dir) {
                fb_print("PoC-OS: load_elf_program: interpreter not found: ");
                fb_print(interp_path);
                fb_print("\n");
                vmm_destroy_address_space(pml4);
                return 0;
            }
            uint8_t *disk_data = (uint8_t *)kmalloc(interp_file.size);
            if (disk_data == NULL ||
                    vfs_read(&interp_file, 0, disk_data, interp_file.size) != (int64_t)interp_file.size) {
                fb_print("PoC-OS: load_elf_program: failed to read the interpreter.\n");
                if (disk_data != NULL) {
                    kfree(disk_data);
                }
                vmm_destroy_address_space(pml4);
                return 0;
            }
            interp_data = disk_data;
            interp_size = interp_file.size;
            owns_interp_data = 1;
        }

        struct elf_load_result interp_elf;
        int loaded = elf_load(pml4, interp_data, interp_size, ELF_INTERP_BASE, &interp_elf);
        if (owns_interp_data) {
            kfree((void *)interp_data); /* elf_load() has already copied whatever it needs into the new address space's own frames */
        }
        if (!loaded) {
            vmm_destroy_address_space(pml4);
            return 0;
        }
        at_base = ELF_INTERP_BASE;
        jump_entry = interp_elf.entry;
    }

    if (!elf_map_user_stack(pml4)) {
        vmm_destroy_address_space(pml4);
        return 0;
    }
    main_elf.stack_top = VMM_USER_STACK_TOP;

    uint64_t rsp = elf_build_user_stack(pml4, &main_elf, at_base, argc, argv, envc, envp);
    if (rsp == 0) {
        vmm_destroy_address_space(pml4);
        return 0;
    }

    *out_entry = jump_entry;
    *out_rsp = rsp;
    return pml4;
}

uint64_t process_create_from_elf(const uint8_t *data, uint64_t size,
                                  int argc, const char *const argv[],
                                  int envc, const char *const envp[]) {
    uint64_t entry, rsp;
    /* Boot-spawned init always effectively runs from "/" -- there's no
     * process context (and so no p->cwd) yet to resolve a relative
     * PT_INTERP path against. */
    uint64_t pml4 = load_elf_program(data, size, "/", argc, argv, envc, envp, &entry, &rsp);
    if (pml4 == 0) {
        fb_print("PoC-OS: process_create_from_elf: failed to load the ELF image.\n");
        return 0;
    }

    uint64_t pid = process_spawn(pml4, entry, rsp);
    if (pid == 0) {
        vmm_destroy_address_space(pml4);
    }
    return pid;
}

/* Linear search for the process with this pid, or NULL if it's not
 * running (already exited and reaped, or never existed). */
static struct process *find_by_pid(uint64_t pid) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state != PROCESS_UNUSED && processes[i].pid == pid) {
            return &processes[i];
        }
    }
    return NULL;
}

/* Shared teardown for a process that's finished running, whether via
 * SYS_EXIT (process_exit_current()) or a default-terminating signal
 * (schedule(), below): releases every pipe end it still held open (the
 * other end's reader/writer count needs to see it's gone, whether or not
 * it ever called close() itself), destroys its address space and kernel
 * stack, and either frees its slot immediately (parent_pid == 0 -- boot-
 * spawned, so nobody could ever waitpid() for it) or turns it into a
 * zombie carrying `status` until reaped, notifying the parent via
 * SIGCHLD either way there *is* one to notify. */
static void terminate_process(struct process *p, int status) {
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (p->fds[i].in_use && (p->fds[i].console_kind == 3 || p->fds[i].console_kind == 4)) {
            struct pipe *pp = p->fds[i].pipe;
            if (p->fds[i].console_kind == 3) {
                pp->readers--;
            } else {
                pp->writers--;
            }
            if (pp->readers == 0 && pp->writers == 0) {
                kfree(pp);
            }
            p->fds[i].in_use = 0;
        }
    }
    vmm_destroy_address_space(p->pml4_phys);
    kfree(p->kernel_stack);
    p->kernel_stack = NULL;

    if (p->parent_pid != 0) {
        struct process *parent = find_by_pid(p->parent_pid);
        if (parent != NULL) {
            parent->sig_pending |= (1ull << SIGCHLD);
        }
    }

    if (p->parent_pid == 0) {
        p->state = PROCESS_UNUSED;
    } else {
        p->exit_status = status;
        p->state = PROCESS_ZOMBIE;
    }
}

static int default_action_terminates(int sig) {
    return sig != SIGCHLD; /* SIGCHLD's default action is "ignore"; everything else we define terminates */
}

/* Applies whatever signals are pending-and-unblocked on `p` before it's
 * allowed to actually run: SIGKILL always terminates (never blockable,
 * never catchable -- checked first, ignoring sig_blocked entirely); of
 * the rest, at most one deliverable signal is handled per call (ignored,
 * defaulted, or dispatched to a handler via a trampoline frame built on
 * `p`'s own user stack -- see SYS_SIGACTION's doc comment), since running
 * the handler is itself enough progress for one scheduling opportunity.
 * Returns 0 if `p` is still alive afterward (regs is ready to iretq into,
 * whether unchanged, or redirected into a handler), or the signal number
 * that terminated it (regs is left untouched in that case; the caller
 * owns tearing p down via terminate_process() and picking someone else
 * to run). */
static int deliver_pending_signals(struct process *p, struct registers *regs) {
    if (p->sig_pending & (1ull << SIGKILL)) {
        return SIGKILL;
    }

    uint64_t deliverable = p->sig_pending & ~p->sig_blocked;
    for (int sig = 1; sig < POC_NSIG; sig++) {
        if (!(deliverable & (1ull << sig))) {
            continue;
        }
        p->sig_pending &= ~(1ull << sig);

        uint64_t handler = p->sig_handlers[sig];
        if (handler == POC_SIG_IGN) {
            continue;
        }
        if (handler == POC_SIG_DFL) {
            if (default_action_terminates(sig)) {
                return sig;
            }
            continue;
        }

        /* Real handler: save the full interrupted context onto p's own
         * stack, then redirect regs to enter it as if `handler(sig)` had
         * just been called with sig_restorer as the return address --
         * SYS_SIGRETURN (invoked by sig_restorer) copies the saved
         * context back over *regs wholesale once the handler returns. */
        uint64_t sigframe_addr = (regs->rsp - sizeof(struct registers)) & ~(uint64_t)0xF;
        uint64_t new_rsp = sigframe_addr - 8;
        if (!copy_to_user(p->pml4_phys, sigframe_addr, regs, sizeof(struct registers)) ||
            !copy_to_user(p->pml4_phys, new_rsp, &p->sig_restorer, 8)) {
            continue; /* can't build the frame -- drop the signal rather than corrupting p */
        }
        regs->rsp = new_rsp;
        regs->rdi = (uint64_t)sig;
        regs->rip = handler;
        return 0;
    }
    return 0;
}

/* Shared by scheduler_tick() (timer-driven) and process_exit_current()
 * (syscall-driven): save whoever's running (if still running -- an
 * exiting process has already been marked UNUSED by the caller, so it's
 * skipped here), then round-robin to the next READY slot willing to
 * actually run (deliver_pending_signals() may terminate candidates along
 * the way -- see its doc comment). */
static void schedule(struct registers *regs) {
    if (current_index != -1 && processes[current_index].state == PROCESS_RUNNING) {
        processes[current_index].regs = *regs;
        fpu_save(processes[current_index].fpu_state);
        processes[current_index].state = PROCESS_READY;
    }

    int start = (current_index == -1) ? 0 : current_index;
    for (int offset = 1; offset <= PROCESS_MAX; offset++) {
        int i = (start + offset) % PROCESS_MAX;
        if (processes[i].state != PROCESS_READY) {
            continue;
        }
        current_index = i;
        processes[i].state = PROCESS_RUNNING;
        *regs = processes[i].regs;

        int killed_by = deliver_pending_signals(&processes[i], regs);
        if (killed_by != 0) {
            terminate_process(&processes[i], 128 + killed_by); /* POSIX's "128+signum" exit-status convention */
            current_index = -1;
            continue;
        }

        tss_set_kernel_stack((uint64_t)processes[i].kernel_stack + PROCESS_KERNEL_STACK_SIZE);
        vmm_switch_address_space(processes[i].pml4_phys);
        wrmsr(IA32_FS_BASE_MSR, processes[i].fs_base);
        fpu_restore(processes[i].fpu_state);
        return;
    }
    /* Nothing READY. The loop above always finds at least whoever was
     * RUNNING on entry (re-marked READY right before it, unless a
     * default-terminating signal just killed it) -- so reaching this
     * point means current_index was already -1 when we were called,
     * which only happens from process_exit_current() once the very last
     * process is gone (or a cascade of signal-killed candidates leaves
     * nothing behind). Either way, *regs at this point belongs to a
     * process whose address space/kernel stack terminate_process()
     * already freed -- resuming it (letting the caller's POP_ALL+iretq
     * run normally) would run that freed address space, not idle
     * safely. Halting here instead, on this same kernel stack, avoids
     * ever returning into that stale context. */
    fb_print("PoC-OS: no runnable processes left -- halting.\n");
    for (;;) {
        asm volatile ("hlt");
    }
}

void scheduler_tick(struct registers *regs) {
    schedule(regs);
}

void process_exit_current(struct registers *regs, int status) {
    if (current_index != -1) {
        terminate_process(&processes[current_index], status);
        current_index = -1;
    }
    schedule(regs);
}

uint64_t process_fork(struct registers *regs) {
    struct process *parent = current();
    if (parent == NULL) {
        return 0;
    }

    uint64_t child_pml4 = vmm_clone_address_space(parent->pml4_phys);
    if (child_pml4 == 0) {
        return 0;
    }

    uint8_t *child_stack = (uint8_t *)kmalloc(PROCESS_KERNEL_STACK_SIZE);
    if (child_stack == NULL) {
        vmm_destroy_address_space(child_pml4);
        return 0;
    }

    int slot = -1;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROCESS_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        kfree(child_stack);
        vmm_destroy_address_space(child_pml4);
        return 0;
    }

    struct process *c = &processes[slot];
    c->pid = next_pid++;
    c->pml4_phys = child_pml4;
    c->kernel_stack = child_stack;
    c->next_anon_va = parent->next_anon_va;
    c->fs_base = parent->fs_base;
    /* vmm_clone_address_space() already gave the child its own private
     * copy of every mapped page, anon regions included -- copying the
     * bookkeeping here just lets the child's own future munmap() calls
     * find and free ITS copies of those frames correctly. */
    memcpy(c->vmas, parent->vmas, sizeof(c->vmas));
    memcpy(c->cwd, parent->cwd, sizeof(c->cwd));
    memcpy(c->fds, parent->fds, sizeof(c->fds));
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!c->fds[i].in_use) {
            continue;
        }
        if (c->fds[i].console_kind == 3) {
            c->fds[i].pipe->readers++;
        } else if (c->fds[i].console_kind == 4) {
            c->fds[i].pipe->writers++;
        }
    }
    c->parent_pid = parent->pid;
    c->exit_status = 0;

    /* POSIX fork(): handlers, the blocked-signal mask, and the restorer
     * are all inherited; pending signals are not -- they're a per-process
     * notification of something that already happened to the parent, not
     * shared state to hand the child a copy of. */
    c->sig_pending = 0;
    c->sig_blocked = parent->sig_blocked;
    memcpy(c->sig_handlers, parent->sig_handlers, sizeof(c->sig_handlers));
    c->sig_restorer = parent->sig_restorer;

    c->regs = *regs;
    c->regs.rax = 0; /* the child's own eventual "fork() returned" value */
    /* fpu_save() here captures the *parent's* live FPU/SSE hardware
     * state directly (fork() runs synchronously as the parent, so
     * nothing has context-switched away yet) -- correct starting point
     * for the child too, per POSIX fork() semantics. */
    fpu_save(c->fpu_state);

    c->state = PROCESS_READY;

    return c->pid;
}

int process_execve(struct registers *regs, const char *path,
                    int argc, const char *const argv[],
                    int envc, const char *const envp[]) {
    struct process *p = current();
    if (p == NULL) {
        return 0;
    }

    /* Tried first, same as spawn_boot_program()'s /busybox lookup in
     * main.c: BusyBox's own standalone-shell dispatch re-execs "/busybox"
     * itself (with a different argv[0]) for every applet that isn't
     * NOFORK, so this path gets hit constantly, not just at boot -- and
     * unlike vfs_open()'s backend (the real FAT32 disk, or ramfs.c with
     * no disk attached), tarfs's initrd is guaranteed to actually have
     * it. Only if it's not there does this fall back to vfs_open()/
     * vfs_read(), same as any other path. */
    uint64_t size;
    const uint8_t *data;
    int owns_data;

    const char *tarfs_name = (path[0] == '/') ? path + 1 : path;
    data = tarfs_read(tarfs_name, &size);
    if (data != NULL) {
        owns_data = 0;
    } else {
        struct fat32_file exe;
        if (!vfs_open(p->cwd, path, O_RDONLY, &exe) || exe.is_dir) {
            return 0;
        }
        /* elf_load() needs the whole image as one contiguous in-memory
         * buffer (it memcpy()s straight out of it into each PT_LOAD
         * segment's frames) -- unlike tarfs's already-in-RAM initrd, a
         * fat32-/ramfs-backed file has to actually be read out via
         * vfs_read() first. */
        uint8_t *disk_data = (uint8_t *)kmalloc(exe.size);
        if (disk_data == NULL) {
            return 0;
        }
        if (vfs_read(&exe, 0, disk_data, exe.size) != (int64_t)exe.size) {
            kfree(disk_data);
            return 0;
        }
        data = disk_data;
        size = exe.size;
        owns_data = 1;
    }

    uint64_t entry, rsp;
    uint64_t new_pml4 = load_elf_program(data, size, p->cwd, argc, argv, envc, envp, &entry, &rsp);
    if (owns_data) {
        kfree((void *)data); /* elf_load() has already copied whatever it needs into the new address space's own frames */
    }
    if (new_pml4 == 0) {
        return 0;
    }

    /* Everything above can still fail without disturbing the running
     * process -- only now do we commit, switching CR3 to the new address
     * space before freeing the old one so CR3 never points at frames
     * that have already been handed back to the PMM. */
    uint64_t old_pml4 = p->pml4_phys;
    p->pml4_phys = new_pml4;
    p->next_anon_va = VMM_USER_ANON_BASE;
    p->fs_base = 0;
    /* POSIX execve() resets the FPU/SSE state along with everything
     * else about the old image; fpu_restore() below (once this process
     * is scheduled back in) will load this fresh copy into hardware. */
    memcpy(p->fpu_state, fpu_default_state, sizeof(p->fpu_state));
    /* The old anon mappings belonged to an address space we're about to
     * destroy -- there's nothing left for a stale vmas[] entry to
     * describe. */
    for (int i = 0; i < PROCESS_VMA_MAX; i++) {
        p->vmas[i].in_use = 0;
    }
    /* POSIX execve(): a caught signal's handler address belonged to the
     * old, now-gone image, so it resets to the default action; SIG_IGN
     * and the blocked-signal mask survive (they're properties of the
     * process, not the image). The old restorer is equally meaningless
     * now -- the new image will install its own on its first
     * SYS_SIGACTION call, same as any process's first one ever. */
    for (int i = 0; i < POC_NSIG; i++) {
        if (p->sig_handlers[i] != POC_SIG_IGN) {
            p->sig_handlers[i] = POC_SIG_DFL;
        }
    }
    p->sig_restorer = 0;
    vmm_switch_address_space(new_pml4);
    wrmsr(IA32_FS_BASE_MSR, 0);
    vmm_destroy_address_space(old_pml4);

    regs->rip = entry;
    regs->rsp = rsp;
    regs->rax = regs->rbx = regs->rcx = regs->rdx = 0;
    regs->rsi = regs->rdi = regs->rbp = 0;
    regs->r8 = regs->r9 = regs->r10 = regs->r11 = 0;
    regs->r12 = regs->r13 = regs->r14 = regs->r15 = 0;
    /* cs/rflags/rsp(set above)/ss are already correct ring3 values from
     * the trapped context this process was already running in -- exec
     * doesn't change privilege level or re-enable interrupts, so nothing
     * else needs touching. */

    return 1;
}

int64_t process_waitpid(int64_t target_pid, int *out_status) {
    struct process *parent = current();
    if (parent == NULL) {
        return -1;
    }

    int found_child = 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process *c = &processes[i];
        if (c->state == PROCESS_UNUSED || c->parent_pid != parent->pid) {
            continue;
        }
        if (target_pid != -1 && (int64_t)c->pid != target_pid) {
            continue;
        }
        found_child = 1;
        if (c->state == PROCESS_ZOMBIE) {
            uint64_t pid = c->pid;
            if (out_status != NULL) {
                *out_status = c->exit_status;
            }
            c->state = PROCESS_UNUSED;
            return (int64_t)pid;
        }
    }
    return found_child ? 0 : -1;
}

/* Returns the currently running process's pid, or 0 if none is running. */
uint64_t process_current_pid(void) {
    struct process *p = current();
    return (p == NULL) ? 0 : p->pid;
}

/* Returns the currently running process's parent's pid (0 = boot-spawned,
 * no parent). */
uint64_t process_current_ppid(void) {
    struct process *p = current();
    return (p == NULL) ? 0 : p->parent_pid;
}

/* Returns the currently running process's address space (its PML4
 * physical address) -- syscall.c passes this to usercopy.c's
 * copy_from_user()/copy_to_user() to validate syscall pointer arguments. */
uint64_t process_current_pml4(void) {
    struct process *p = current();
    return (p == NULL) ? 0 : p->pml4_phys;
}

/* Opens `path` (resolved against the current process's cwd) and installs
 * it as a fat32-backed fd in the first free slot. A plain read-only open
 * of an absolute path is tried against the initrd (tarfs.c) first --
 * same fallback main.c's spawn_boot_program()/this file's
 * load_elf_program() already apply to /busybox and its own PT_INTERP at
 * boot, needed here too since mlibc's rtld resolves further DT_NEEDED
 * dependencies (e.g. /lib/libc.so) via ordinary open() calls at runtime,
 * not through the kernel's own ELF loader. Anything that wants to
 * write/create/truncate always goes straight to the real (FAT32)
 * filesystem, since tarfs.c is read-only. Returns the new fd number, or
 * -1 (file not found, or no free fd slots). */
int process_fd_open(const char *path, int flags) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }

    if (path[0] == '/' && !(flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))) {
        uint64_t tar_size;
        const uint8_t *tar_data = tarfs_read(path + 1, &tar_size);
        if (tar_data != NULL) {
            for (int i = 0; i < PROCESS_FD_MAX; i++) {
                if (!p->fds[i].in_use) {
                    p->fds[i].in_use = 1;
                    p->fds[i].console_kind = 5;
                    p->fds[i].tarfs_data = tar_data;
                    p->fds[i].size = tar_size;
                    p->fds[i].offset = 0;
                    p->fds[i].mode = 0100000u; /* S_IFREG */
                    p->fds[i].writable = 0;
                    return i;
                }
            }
            return -1;
        }
    }

    struct fat32_file file;
    if (!vfs_open(p->cwd, path, flags, &file)) {
        return -1;
    }
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!p->fds[i].in_use) {
            p->fds[i].in_use = 1;
            p->fds[i].console_kind = 0;
            p->fds[i].fatfile = file;
            p->fds[i].size = file.size;
            /* O_APPEND: start writing at the file's current end rather
             * than its start -- there's only one writer per fd in this
             * kernel (no concurrent-append race to worry about), so
             * setting this once at open time is enough; process_fd_write()
             * advances it normally from here on. */
            p->fds[i].offset = (flags & O_APPEND) ? file.size : 0;
            p->fds[i].mode = file.is_dir ? 0040000u /* S_IFDIR */ : 0100000u /* S_IFREG */;
            p->fds[i].writable = (flags & (O_WRONLY | O_RDWR)) != 0;
            return i;
        }
    }
    return -1;
}

static struct process_fd *fd_lookup(struct process *p, int fd) {
    if (p == NULL || fd < 0 || fd >= PROCESS_FD_MAX || !p->fds[fd].in_use) {
        return NULL;
    }
    return &p->fds[fd];
}

/* Returns the write position of a pipe's ring buffer -- derived rather
 * than stored, since read_pos + count (mod the buffer size) always gives
 * it and keeping only two of the three in sync is one less place for
 * them to drift apart. */
static uint64_t pipe_write_pos(struct pipe *p) {
    return (p->read_pos + p->count) % PIPE_BUF_SIZE;
}

int64_t process_fd_read(int fd, void *kbuf, uint64_t len) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL) {
        return -1;
    }
    if (f->console_kind == 1) {
        /* Same "poll again" convention as a pipe with no data but writers
         * still open (below): a bare 0 would be indistinguishable from
         * real EOF to a userspace read() wrapper that (correctly) retries
         * on the pipe's -2 sentinel but not on stdin's own 0. Real EOF
         * only ever happens via Ctrl-D on an empty line (see
         * console_consume_eof()) -- otherwise (nothing typed, and no
         * pending Ctrl-D) this is "poll again", not EOF. */
        uint64_t n = console_read_nonblock((uint8_t *)kbuf, len);
        if (n > 0) {
            return (int64_t)n;
        }
        return console_consume_eof() ? 0 : -2;
    }
    if (f->console_kind == 2) {
        return -1; /* stdout/stderr aren't readable */
    }
    if (f->console_kind == 4) {
        return -1; /* pipe write ends aren't readable */
    }
    if (f->console_kind == 3) {
        struct pipe *p = f->pipe;
        if (p->count == 0) {
            return (p->writers == 0) ? 0 : -2; /* true EOF vs. "poll again" */
        }
        uint64_t n = (len < p->count) ? len : p->count;
        uint8_t *dst = (uint8_t *)kbuf;
        for (uint64_t i = 0; i < n; i++) {
            dst[i] = p->buf[(p->read_pos + i) % PIPE_BUF_SIZE];
        }
        p->read_pos = (p->read_pos + n) % PIPE_BUF_SIZE;
        p->count -= n;
        return (int64_t)n;
    }
    if (f->console_kind == 5) {
        uint64_t remaining = f->size - f->offset;
        uint64_t n = (len < remaining) ? len : remaining;
        memcpy(kbuf, f->tarfs_data + f->offset, n);
        f->offset += n;
        return (int64_t)n;
    }
    int64_t n = vfs_read(&f->fatfile, f->offset, kbuf, len);
    if (n > 0) {
        f->offset += (uint64_t)n;
    }
    return n;
}

int64_t process_fd_write(int fd, const void *kbuf, uint64_t len) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL) {
        return -1;
    }
    if (f->console_kind == 2) {
        const uint8_t *bytes = (const uint8_t *)kbuf;
        for (uint64_t i = 0; i < len; i++) {
            fb_putc((char)bytes[i]);
        }
        return (int64_t)len;
    }
    if (f->console_kind == 4) {
        struct pipe *p = f->pipe;
        if (p->readers == 0) {
            return -1; /* broken pipe -- no SIGPIPE support, just an error */
        }
        uint64_t space = PIPE_BUF_SIZE - p->count;
        if (space == 0) {
            return -2; /* full -- "poll again", not an error */
        }
        uint64_t n = (len < space) ? len : space; /* a short write is valid pipe behavior */
        const uint8_t *src = (const uint8_t *)kbuf;
        uint64_t wpos = pipe_write_pos(p);
        for (uint64_t i = 0; i < n; i++) {
            p->buf[(wpos + i) % PIPE_BUF_SIZE] = src[i];
        }
        p->count += n;
        return (int64_t)n;
    }
    if (!f->writable) {
        return -1; /* stdin and pipe read ends are always read-only; a fat32/ramfs fd only if opened that way */
    }
    int64_t n = vfs_write(&f->fatfile, f->offset, kbuf, len);
    if (n > 0) {
        f->offset += (uint64_t)n;
        f->size = f->fatfile.size;
    }
    return n;
}

/* Drops this fd's reference to whatever it points at -- for a pipe end,
 * decrementing the matching refcount and freeing the shared struct pipe
 * once both ends have no fd left referencing them (process_fork() is the
 * only other place that adds references, via process_fd_close()'s
 * mirror-image increment there). */
int process_fd_close(int fd) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL) {
        return -1;
    }
    if (f->console_kind == 3 || f->console_kind == 4) {
        struct pipe *p = f->pipe;
        if (f->console_kind == 3) {
            p->readers--;
        } else {
            p->writers--;
        }
        if (p->readers == 0 && p->writers == 0) {
            kfree(p);
        }
    }
    f->in_use = 0;
    return 0;
}

/* Creates a new pipe: two freshly opened fds in the calling process
 * (*out_read_fd, *out_write_fd), backed by one shared struct pipe with
 * one reader and one writer. Returns 0 on success, -1 on failure (out of
 * memory, or fewer than 2 free fd slots -- any fd opened before the
 * failure is rolled back). */
int process_pipe_create(int *out_read_fd, int *out_write_fd) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }

    struct pipe *pipe = (struct pipe *)kmalloc(sizeof(struct pipe));
    if (pipe == NULL) {
        return -1;
    }
    pipe->read_pos = 0;
    pipe->count = 0;
    pipe->readers = 1;
    pipe->writers = 1;

    int read_fd = -1, write_fd = -1;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!p->fds[i].in_use) {
            read_fd = i;
            break;
        }
    }
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!p->fds[i].in_use && i != read_fd) {
            write_fd = i;
            break;
        }
    }
    if (read_fd == -1 || write_fd == -1) {
        kfree(pipe);
        return -1;
    }

    p->fds[read_fd].in_use = 1;
    p->fds[read_fd].console_kind = 3;
    p->fds[read_fd].pipe = pipe;
    p->fds[write_fd].in_use = 1;
    p->fds[write_fd].console_kind = 4;
    p->fds[write_fd].pipe = pipe;

    *out_read_fd = read_fd;
    *out_write_fd = write_fd;
    return 0;
}

/* Makes `newfd` refer to whatever `oldfd` refers to in the calling
 * process (closing whatever `newfd` used to be first, POSIX-style), and
 * bumps the target pipe's refcount if it's a pipe end -- newfd is now a
 * second independent reference to the same underlying struct pipe.
 * Returns newfd, or -1 (oldfd invalid, or newfd out of range). */
int process_fd_dup2(int oldfd, int newfd) {
    struct process *p = current();
    struct process_fd *src = fd_lookup(p, oldfd);
    if (src == NULL || newfd < 0 || newfd >= PROCESS_FD_MAX) {
        return -1;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    if (p->fds[newfd].in_use) {
        process_fd_close(newfd);
    }

    p->fds[newfd] = *src;
    if (src->console_kind == 3) {
        src->pipe->readers++;
    } else if (src->console_kind == 4) {
        src->pipe->writers++;
    }
    return newfd;
}

#define SYS_SEEK_SET 0
#define SYS_SEEK_CUR 1
#define SYS_SEEK_END 2

/* Moves fd's read/write offset, POSIX lseek()-style: SEEK_SET is
 * relative to the start of the file, SEEK_CUR to the current offset,
 * SEEK_END to the file's current size. Only fat32-backed (0) and
 * tarfs-backed (5) fds support seeking. Returns the new offset, or -1 on
 * an invalid fd/whence/result. */
int64_t process_fd_lseek(int fd, int64_t offset, int whence) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL || (f->console_kind != 0 && f->console_kind != 5)) {
        return -1;
    }
    int64_t base;
    if (whence == SYS_SEEK_SET) {
        base = 0;
    } else if (whence == SYS_SEEK_CUR) {
        base = (int64_t)f->offset;
    } else if (whence == SYS_SEEK_END) {
        base = (int64_t)f->size;
    } else {
        return -1;
    }
    int64_t new_off = base + offset;
    if (new_off < 0 || (uint64_t)new_off > f->size) {
        return -1;
    }
    f->offset = (uint64_t)new_off;
    return new_off;
}

/* Reports fd's size and mode bits, the way fstat() needs to: a pipe
 * reports its buffered byte count and S_IFIFO, a console fd (stdin/
 * stdout/stderr) reports S_IFCHR with size 0, and a real fat32 fd or a
 * tarfs-backed fd (5) reports its actual file size/mode. */
int process_fd_fstat(int fd, uint64_t *out_size, uint32_t *out_mode) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL) {
        return -1;
    }
    if (f->console_kind == 3 || f->console_kind == 4) {
        *out_size = f->pipe->count;
        *out_mode = 0010000; /* S_IFIFO */
        return 0;
    }
    if (f->console_kind == 1 || f->console_kind == 2) {
        *out_size = 0;
        *out_mode = 0020000; /* S_IFCHR */
        return 0;
    }
    *out_size = f->size;
    *out_mode = f->mode;
    return 0;
}

/* fd->offset (normally a byte offset -- see process_fd_read()/
 * process_fd_lseek()) doubles as vfs_readdir()'s slot index for a
 * directory fd instead: there's nothing to "read" from a directory in
 * the regular-file sense, and reusing the same field means SYS_LSEEK's
 * existing SEEK_SET/SEEK_CUR/SEEK_END handling (clamped to f->size,
 * which vfs_open() always reports as 0 for a directory) already gives
 * rewinddir()-via-lseek(fd,0,SEEK_SET) for free, without a second piece
 * of per-fd state. */
int64_t process_fd_getdents(int fd, void *kbuf, uint64_t max_entries) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL || f->console_kind != 0 || !f->fatfile.is_dir) {
        return -1;
    }

    struct poc_dirent *out = (struct poc_dirent *)kbuf;
    uint64_t count = 0;
    while (count < max_entries) {
        char name[13];
        uint32_t size;
        int is_dir;
        int status = vfs_readdir(&f->fatfile, (uint32_t)f->offset, name, &size, &is_dir);
        f->offset++;
        if (status == FAT32_DIRENT_END) {
            break;
        }
        if (status == FAT32_DIRENT_SKIP) {
            continue;
        }
        out[count].mode = is_dir ? 0040000u /* S_IFDIR */ : 0100000u /* S_IFREG */;
        memcpy(out[count].name, name, sizeof(name));
        count++;
    }
    return (int64_t)count;
}

uint64_t process_anon_allocate(uint64_t size) {
    struct process *p = current();
    if (p == NULL || size == 0) {
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < PROCESS_VMA_MAX; i++) {
        if (!p->vmas[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        return 0; /* out of VMA-tracking slots -- can't record this allocation to free later */
    }

    uint64_t pages = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    uint64_t mapped_size = pages * PMM_FRAME_SIZE;
    uint64_t base = p->next_anon_va;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            return 0; /* whatever was mapped so far is leaked -- fine for bring-up */
        }
        vmm_map(p->pml4_phys, base + i * PMM_FRAME_SIZE, frame, VMM_USER | VMM_WRITABLE | VMM_NX);
    }
    p->next_anon_va = base + mapped_size;

    p->vmas[slot].in_use = 1;
    p->vmas[slot].base = base;
    p->vmas[slot].size = mapped_size;
    return base;
}

/* Real munmap: unmaps and frees every physical frame backing the
 * previously process_anon_allocate()'d region starting at exactly
 * `base` (an exact-match requirement, same as a real malloc's mmap-
 * backed large-allocation path only ever munmap()s exactly what it got
 * from mmap()). The virtual address range itself is not reclaimed for
 * reuse -- process_anon_allocate() only ever bumps `next_anon_va`
 * forward -- but the physical memory genuinely comes back, which is the
 * part a libc's malloc actually needs. Returns 0 on success, -1 if no
 * live allocation starts at exactly `base`. */
int process_anon_free(uint64_t base, uint64_t size) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }
    for (int i = 0; i < PROCESS_VMA_MAX; i++) {
        if (p->vmas[i].in_use && p->vmas[i].base == base) {
            uint64_t pages = p->vmas[i].size / PMM_FRAME_SIZE;
            for (uint64_t j = 0; j < pages; j++) {
                uint64_t va = base + j * PMM_FRAME_SIZE;
                uint64_t phys = vmm_translate(p->pml4_phys, va);
                if (phys != UINT64_MAX) {
                    vmm_unmap(p->pml4_phys, va);
                    pmm_free_frame(phys);
                }
            }
            p->vmas[i].in_use = 0;
            (void)size; /* the recorded size is authoritative; a mismatched caller-supplied size doesn't change what's actually mapped */
            return 0;
        }
    }
    return -1;
}

uint64_t process_anon_allocate_fixed(uint64_t vaddr, uint64_t size) {
    struct process *p = current();
    if (p == NULL || size == 0 || (vaddr & (PMM_FRAME_SIZE - 1)) != 0) {
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < PROCESS_VMA_MAX; i++) {
        if (!p->vmas[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        return 0; /* out of VMA-tracking slots */
    }

    uint64_t pages = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    uint64_t mapped_size = pages * PMM_FRAME_SIZE;

    /* Checked as a separate pass before mapping anything -- unlike
     * process_anon_allocate()'s always-fresh watermark, the caller picked
     * this address, so a collision with an already-live mapping (another
     * DSO's segment, the executable, the stack) must fail outright rather
     * than silently overwriting it partway through. */
    for (uint64_t i = 0; i < pages; i++) {
        if (vmm_translate(p->pml4_phys, vaddr + i * PMM_FRAME_SIZE) != UINT64_MAX) {
            return 0;
        }
    }

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            return 0; /* whatever was mapped so far is leaked -- fine for bring-up */
        }
        vmm_map(p->pml4_phys, vaddr + i * PMM_FRAME_SIZE, frame, VMM_USER | VMM_WRITABLE | VMM_NX);
    }

    p->vmas[slot].in_use = 1;
    p->vmas[slot].base = vaddr;
    p->vmas[slot].size = mapped_size;
    return vaddr;
}

/* Changes the read/write/execute permissions of an already-mapped
 * region, POSIX mprotect()-style. `vaddr` must be page-aligned and every
 * page in [vaddr, vaddr+size) must already be mapped, or this fails
 * without changing anything. */
int process_mprotect(uint64_t vaddr, uint64_t size, uint64_t prot) {
    struct process *p = current();
    if (p == NULL || size == 0 || (vaddr & (PMM_FRAME_SIZE - 1)) != 0) {
        return -1;
    }
    uint64_t pages = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    /* Validated up front so a range that's only partially mapped fails
     * cleanly instead of leaving some pages re-permissioned and others
     * not. */
    for (uint64_t i = 0; i < pages; i++) {
        if (vmm_translate(p->pml4_phys, vaddr + i * PMM_FRAME_SIZE) == UINT64_MAX) {
            return -1;
        }
    }

    uint64_t flags = VMM_USER;
    if (prot & PROT_WRITE) {
        flags |= VMM_WRITABLE;
    }
    if (!(prot & PROT_EXEC)) {
        flags |= VMM_NX;
    }
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = vaddr + i * PMM_FRAME_SIZE;
        uint64_t frame = vmm_translate(p->pml4_phys, va) & ~(uint64_t)0xFFF;
        vmm_map(p->pml4_phys, va, frame, flags);
    }
    return 0;
}

/* Sets the FS segment base for the current process, both in its saved
 * state (so a later context switch restores it) and immediately in the
 * live FS.base MSR (so it takes effect right away too) -- used by
 * thread-local storage (TLS) setup, which addresses per-thread data via
 * FS-relative offsets. */
void process_set_fs_base(uint64_t value) {
    struct process *p = current();
    if (p == NULL) {
        return;
    }
    p->fs_base = value;
    wrmsr(IA32_FS_BASE_MSR, value);
}

/* Copies the current process's working directory path into `kbuf`
 * (kernel buffer). Returns the string length (not including the NUL),
 * or -1 if `size` is too small to hold it. */
int process_getcwd(char *kbuf, uint64_t size) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }
    uint64_t len = 0;
    while (p->cwd[len] != '\0') {
        len++;
    }
    if (len + 1 > size) {
        return -1;
    }
    memcpy(kbuf, p->cwd, len + 1);
    return (int)len;
}

/* Changes the current process's working directory to `kpath` (already
 * resolved/validated by the caller -- this just stores it). */
int process_chdir(const char *kpath) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }
    uint64_t len = 0;
    while (kpath[len] != '\0' && len < sizeof(p->cwd) - 1) {
        len++;
    }
    memcpy(p->cwd, kpath, len);
    p->cwd[len] = '\0';
    return 0;
}

int64_t process_sigaction(int sig, uint64_t handler, uint64_t restorer) {
    struct process *p = current();
    if (p == NULL || sig < 0 || sig >= POC_NSIG) {
        return -1;
    }
    int64_t old = (int64_t)p->sig_handlers[sig];
    p->sig_handlers[sig] = handler;
    p->sig_restorer = restorer;
    return old;
}

uint64_t process_sigprocmask(int how, uint64_t mask) {
    struct process *p = current();
    if (p == NULL) {
        return 0;
    }
    uint64_t old = p->sig_blocked;
    if (how == 0) {        /* SIG_BLOCK */
        p->sig_blocked |= mask;
    } else if (how == 1) { /* SIG_UNBLOCK */
        p->sig_blocked &= ~mask;
    } else {                /* SIG_SETMASK (or anything else -- treat as the safe default) */
        p->sig_blocked = mask;
    }
    /* SIGKILL is deliver_pending_signals()'s first, unconditional check --
     * blocking it would only lie to whoever asked for its old state back,
     * since it can never actually take effect. */
    p->sig_blocked &= ~(1ull << SIGKILL);
    return old;
}

int process_send_signal(uint64_t target_pid, int sig) {
    if (sig < 0 || sig >= POC_NSIG) {
        return -1;
    }
    struct process *target = find_by_pid(target_pid);
    if (target == NULL) {
        return -1;
    }
    target->sig_pending |= (1ull << sig);
    return 0;
}

/* Creates a new directory at `kpath`, resolved against the current
 * process's cwd. Returns 0 on success, -1 on failure. */
int process_mkdir(const char *kpath) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }
    return vfs_mkdir(p->cwd, kpath) ? 0 : -1;
}

int process_unlink(const char *kpath) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }
    return vfs_unlink(p->cwd, kpath) ? 0 : -1;
}

int process_rmdir(const char *kpath) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }
    return vfs_rmdir(p->cwd, kpath) ? 0 : -1;
}

int process_rename(const char *kold, const char *knew) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }
    return vfs_rename(p->cwd, kold, knew) ? 0 : -1;
}
