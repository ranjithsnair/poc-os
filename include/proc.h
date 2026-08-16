// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in
// kernel/swtch.asm at the "Switch stacks" comment. Switch doesn't save
// eip/rip explicitly, but it is on the stack (the return address
// pushed by the `call swtch` that got here) and allocproc() manipulates it.
//
// SysV's callee-saved registers: rbx, rbp, r12-r15 (rsp is handled by the
// stack-pointer swap itself, not saved as a field here). eip (not rip)
// for the same reason noted on struct trapframe in x86.h: proc.c
// references context->eip by name.
struct context {
  uint64 r15;
  uint64 r14;
  uint64 r13;
  uint64 r12;
  uint64 rbx;
  uint64 rbp;
  uint64 eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uintp sz;                    // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  uintp tls_base;               // %fs base (SYS_arch_prctl/ARCH_SET_FS);
                                 // reasserted via WRMSR on every return
                                 // to user mode - see kernel/trap.c
  ushort uid,  gid;             // real uid/gid
  ushort euid, egid;            // effective uid/gid - what kernel/fs.c's
                                 // permcheck() actually checks against
  ushort suid, sgid;            // saved-set uid/gid - see kernel/sysproc.c's
                                 // sys_setuid()/sys_setgid() for why these
                                 // are needed for correct POSIX semantics
  ushort umask;                 // file-creation mask (low 9 bits used)
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
