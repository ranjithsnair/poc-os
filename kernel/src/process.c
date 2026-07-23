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
 * CR3/TSS.RSP0) before returning -- the existing POP_ALL+iretq then
 * resumes that process instead. The same trick works for a process's
 * very first run (nothing special needed beyond it being READY) and for
 * SYS_EXIT (process_exit_current() marks the slot unused and reschedules
 * through the same path).
 */
#include <stddef.h>
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "gdt.h"
#include "heap.h"
#include "serial.h"

#define PROCESS_MAX 16
#define PROCESS_KERNEL_STACK_SIZE 16384

enum process_state { PROCESS_UNUSED = 0, PROCESS_READY, PROCESS_RUNNING };

struct process {
    uint64_t pid;
    enum process_state state;
    uint64_t pml4_phys;
    uint8_t *kernel_stack;
    struct registers regs; /* saved context while not RUNNING */
};

static struct process processes[PROCESS_MAX];
static int current_index = -1; /* slot *regs currently belongs to, or -1 */
static uint64_t next_pid = 1;

uint64_t process_create(const uint8_t *code, uint64_t code_size, uint64_t entry_virt) {
    int slot = -1;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROCESS_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        serial_print("PoC-OS: process_create: no free process slots.\n");
        return 0;
    }

    uint64_t pml4 = vmm_create_address_space();
    uint64_t code_phys = pmm_alloc_frame();
    uint64_t stack_phys = pmm_alloc_frame();
    uint8_t *kernel_stack = (uint8_t *)kmalloc(PROCESS_KERNEL_STACK_SIZE);
    if (pml4 == 0 || code_phys == 0 || stack_phys == 0 || kernel_stack == NULL) {
        serial_print("PoC-OS: process_create: out of memory.\n");
        return 0;
    }

    /* Copy the code blob into the fresh frame via the HHDM, then map it
     * (and a stack page right after it) into the new process's own
     * address space -- not the currently active one, which is why
     * vmm_map() takes an explicit pml4_phys. */
    uint8_t *code_kernel_ptr = (uint8_t *)vmm_phys_to_virt(code_phys);
    for (uint64_t i = 0; i < code_size; i++) {
        code_kernel_ptr[i] = code[i];
    }
    vmm_map(pml4, entry_virt, code_phys, VMM_USER);

    uint64_t stack_virt = entry_virt + PMM_FRAME_SIZE;
    vmm_map(pml4, stack_virt, stack_phys, VMM_USER | VMM_WRITABLE | VMM_NX);

    struct process *p = &processes[slot];
    p->pid = next_pid++;
    p->pml4_phys = pml4;
    p->kernel_stack = kernel_stack;

    for (size_t i = 0; i < sizeof(p->regs); i++) {
        ((uint8_t *)&p->regs)[i] = 0;
    }
    /* The saved context a first run needs is identical in shape to what
     * any iretq needs: an entry point, a ring3 code/data selector pair
     * (RPL=3), interrupts enabled, and a stack. General registers start
     * zeroed -- nothing reads them before the process itself sets them. */
    p->regs.rip = entry_virt;
    p->regs.cs = GDT_USER_CODE | 3;
    p->regs.rflags = 0x200; /* IF=1 */
    p->regs.rsp = stack_virt + PMM_FRAME_SIZE;
    p->regs.ss = GDT_USER_DATA | 3;

    /* Flipped last, and last of all the fields the scheduler reads: a
     * tick landing between this line and process_create() returning
     * could pick this process up and run it, but by this point every
     * field it needs (regs, pml4_phys, kernel_stack) is already valid --
     * flip it any earlier and a tick could switch into a half-set-up
     * process (e.g. rip still 0) if a future caller creates a process
     * while the scheduler is already armed. */
    p->state = PROCESS_READY;

    serial_print("PoC-OS: process created.\n");
    return p->pid;
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
        /* Intentionally leaked: pml4_phys, its mapped frames, and
         * kernel_stack. Freeing an address space needs a full page-table
         * walk this kernel doesn't have yet -- fine for a bring-up
         * scheduler running a handful of short-lived test processes. */
        processes[current_index].state = PROCESS_UNUSED;
        current_index = -1;
    }
    schedule(regs);
}
