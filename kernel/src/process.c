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
#include "serial.h"
#include "string.h"
#include "elf.h"
#include "tarfs.h"
#include "console.h"

#define PROCESS_MAX 16
#define PROCESS_KERNEL_STACK_SIZE 16384
#define PROCESS_FD_MAX 8

#define IA32_FS_BASE_MSR 0xC0000100u

static inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

enum process_state { PROCESS_UNUSED = 0, PROCESS_READY, PROCESS_RUNNING };

/* console_kind: 0 = regular file (tarfs-backed), 1 = stdin, 2 = stdout/
 * stderr. There's no writable filesystem, so a regular fd is always
 * read-only tarfs data; the console is the only writable fd kind. */
struct process_fd {
    int in_use;
    int console_kind;
    const uint8_t *data;
    uint64_t size;
    uint64_t offset;
    uint32_t mode;
};

struct process {
    uint64_t pid;
    enum process_state state;
    uint64_t pml4_phys;
    uint8_t *kernel_stack;
    struct registers regs; /* saved context while not RUNNING */
    struct process_fd fds[PROCESS_FD_MAX];
    uint64_t next_anon_va;
    uint64_t fs_base;
    char cwd[64];
};

static struct process processes[PROCESS_MAX];
static int current_index = -1; /* slot *regs currently belongs to, or -1 */
static uint64_t next_pid = 1;

static struct process *current(void) {
    return (current_index == -1) ? NULL : &processes[current_index];
}

/* Shared tail of process_create()/process_create_from_elf(): finds a
 * free slot, allocates the kernel stack, seeds the fd table with the
 * console, and fills in the initial saved-register state an iretq needs
 * to enter ring3 at `entry` with `stack_top` as %rsp. Does NOT touch the
 * address space itself -- callers must have already mapped everything
 * the process needs into `pml4` before calling this. */
static uint64_t process_spawn(uint64_t pml4, uint64_t entry, uint64_t stack_top) {
    int slot = -1;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROCESS_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        serial_print("PoC-OS: process_spawn: no free process slots.\n");
        return 0;
    }

    uint8_t *kernel_stack = (uint8_t *)kmalloc(PROCESS_KERNEL_STACK_SIZE);
    if (kernel_stack == NULL) {
        serial_print("PoC-OS: process_spawn: out of memory.\n");
        return 0;
    }

    struct process *p = &processes[slot];
    p->pid = next_pid++;
    p->pml4_phys = pml4;
    p->kernel_stack = kernel_stack;
    p->next_anon_va = VMM_USER_ANON_BASE;
    p->fs_base = 0;
    p->cwd[0] = '/';
    p->cwd[1] = '\0';

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
     * zeroed -- process_create()'s flat blob never read them before
     * setting its own, and an ELF's crt0 reads argc/argv off the stack,
     * not out of registers, so zeroing here is still correct for both. */
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

    serial_print("PoC-OS: process created.\n");
    return p->pid;
}

uint64_t process_create(const uint8_t *code, uint64_t code_size, uint64_t entry_virt) {
    uint64_t pml4 = vmm_create_address_space();
    uint64_t code_phys = pmm_alloc_frame();
    uint64_t stack_phys = pmm_alloc_frame();
    if (pml4 == 0 || code_phys == 0 || stack_phys == 0) {
        serial_print("PoC-OS: process_create: out of memory.\n");
        return 0;
    }

    /* Copy the code blob into the fresh frame via the HHDM, then map it
     * (and a stack page right after it) into the new process's own
     * address space -- not the currently active one, which is why
     * vmm_map() takes an explicit pml4_phys. */
    uint8_t *code_kernel_ptr = (uint8_t *)vmm_phys_to_virt(code_phys);
    memcpy(code_kernel_ptr, code, code_size);
    vmm_map(pml4, entry_virt, code_phys, VMM_USER);

    uint64_t stack_virt = entry_virt + PMM_FRAME_SIZE;
    vmm_map(pml4, stack_virt, stack_phys, VMM_USER | VMM_WRITABLE | VMM_NX);

    uint64_t pid = process_spawn(pml4, entry_virt, stack_virt + PMM_FRAME_SIZE);
    if (pid == 0) {
        vmm_destroy_address_space(pml4);
    }
    return pid;
}

uint64_t process_create_from_elf(const uint8_t *data, uint64_t size,
                                  int argc, const char *const argv[],
                                  int envc, const char *const envp[]) {
    uint64_t pml4 = vmm_create_address_space();
    if (pml4 == 0) {
        serial_print("PoC-OS: process_create_from_elf: out of memory.\n");
        return 0;
    }

    struct elf_load_result elf;
    if (!elf_load(pml4, data, size, &elf)) {
        vmm_destroy_address_space(pml4);
        return 0;
    }

    uint64_t rsp = elf_build_user_stack(pml4, elf.stack_top, argc, argv, envc, envp);
    if (rsp == 0) {
        serial_print("PoC-OS: process_create_from_elf: failed to build the initial stack.\n");
        vmm_destroy_address_space(pml4);
        return 0;
    }

    uint64_t pid = process_spawn(pml4, elf.entry, rsp);
    if (pid == 0) {
        vmm_destroy_address_space(pml4);
    }
    return pid;
}

/* Shared by scheduler_tick() (timer-driven) and process_exit_current()
 * (syscall-driven): save whoever's running (if still running -- an
 * exiting process has already been marked UNUSED by the caller, so it's
 * skipped here), then round-robin to the next READY slot. */
static void schedule(struct registers *regs) {
    if (current_index != -1 && processes[current_index].state == PROCESS_RUNNING) {
        processes[current_index].regs = *regs;
        processes[current_index].state = PROCESS_READY;
    }

    int start = (current_index == -1) ? 0 : current_index;
    for (int offset = 1; offset <= PROCESS_MAX; offset++) {
        int i = (start + offset) % PROCESS_MAX;
        if (processes[i].state == PROCESS_READY) {
            current_index = i;
            processes[i].state = PROCESS_RUNNING;
            *regs = processes[i].regs;
            tss_set_kernel_stack((uint64_t)processes[i].kernel_stack + PROCESS_KERNEL_STACK_SIZE);
            vmm_switch_address_space(processes[i].pml4_phys);
            wrmsr(IA32_FS_BASE_MSR, processes[i].fs_base);
            return;
        }
    }
    /* Nothing READY: leave *regs as whatever was already interrupted
     * (or, if current_index is now -1, whatever ring0 context called
     * in -- the idle loop keeps spinning). */
}

void scheduler_tick(struct registers *regs) {
    schedule(regs);
}

void process_exit_current(struct registers *regs) {
    if (current_index != -1) {
        struct process *p = &processes[current_index];
        vmm_destroy_address_space(p->pml4_phys);
        kfree(p->kernel_stack);
        p->state = PROCESS_UNUSED;
        current_index = -1;
    }
    schedule(regs);
}

uint64_t process_current_pid(void) {
    struct process *p = current();
    return (p == NULL) ? 0 : p->pid;
}

uint64_t process_current_pml4(void) {
    struct process *p = current();
    return (p == NULL) ? 0 : p->pml4_phys;
}

int process_fd_open(const char *path) {
    struct process *p = current();
    if (p == NULL) {
        return -1;
    }
    uint64_t size;
    uint32_t mode;
    char typeflag;
    const uint8_t *data = tarfs_stat(path, &size, &mode, &typeflag);
    if (data == NULL) {
        return -1;
    }
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!p->fds[i].in_use) {
            p->fds[i].in_use = 1;
            p->fds[i].console_kind = 0;
            p->fds[i].data = data;
            p->fds[i].size = size;
            p->fds[i].offset = 0;
            p->fds[i].mode = mode;
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

int64_t process_fd_read(int fd, void *kbuf, uint64_t len) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL) {
        return -1;
    }
    if (f->console_kind == 1) {
        return (int64_t)console_read_nonblock((uint8_t *)kbuf, len);
    }
    if (f->console_kind == 2) {
        return -1; /* stdout/stderr aren't readable */
    }
    uint64_t remaining = f->size - f->offset;
    uint64_t n = (len < remaining) ? len : remaining;
    memcpy(kbuf, f->data + f->offset, n);
    f->offset += n;
    return (int64_t)n;
}

int64_t process_fd_write(int fd, const void *kbuf, uint64_t len) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL) {
        return -1;
    }
    if (f->console_kind == 2) {
        const uint8_t *bytes = (const uint8_t *)kbuf;
        for (uint64_t i = 0; i < len; i++) {
            serial_putc((char)bytes[i]);
        }
        return (int64_t)len;
    }
    return -1; /* stdin and tarfs-backed fds are read-only */
}

int process_fd_close(int fd) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL) {
        return -1;
    }
    f->in_use = 0;
    return 0;
}

#define SYS_SEEK_SET 0
#define SYS_SEEK_CUR 1
#define SYS_SEEK_END 2

int64_t process_fd_lseek(int fd, int64_t offset, int whence) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL || f->console_kind != 0) {
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

int process_fd_fstat(int fd, uint64_t *out_size, uint32_t *out_mode) {
    struct process_fd *f = fd_lookup(current(), fd);
    if (f == NULL) {
        return -1;
    }
    if (f->console_kind != 0) {
        *out_size = 0;
        *out_mode = 0020000; /* S_IFCHR */
        return 0;
    }
    *out_size = f->size;
    *out_mode = f->mode;
    return 0;
}

uint64_t process_anon_allocate(uint64_t size) {
    struct process *p = current();
    if (p == NULL || size == 0) {
        return 0;
    }
    uint64_t pages = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    uint64_t base = p->next_anon_va;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            return 0; /* whatever was mapped so far is leaked -- fine for bring-up */
        }
        vmm_map(p->pml4_phys, base + i * PMM_FRAME_SIZE, frame, VMM_USER | VMM_WRITABLE | VMM_NX);
    }
    p->next_anon_va = base + pages * PMM_FRAME_SIZE;
    return base;
}

void process_set_fs_base(uint64_t value) {
    struct process *p = current();
    if (p == NULL) {
        return;
    }
    p->fs_base = value;
    wrmsr(IA32_FS_BASE_MSR, value);
}

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
