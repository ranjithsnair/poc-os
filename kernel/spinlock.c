// Mutual exclusion spin locks.
//
// A spinlock protects a short critical section: a CPU that can't acquire
// the lock just spins (busy-waits) in a tight loop instead of blocking,
// so spinlocks should only ever be held for a few instructions - never
// across a call that might sleep (see sleeplock.c for that case, e.g.
// while waiting on disk I/O).

#include "types.h"
#include "defs.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
// Holding a lock for a long time may cause
// other CPUs to waste time spinning to acquire it.
void
acquire(struct spinlock *lk)
{
  // Disable interrupts to avoid deadlock: if a timer interrupt fired on
  // this CPU while we were spinning, and its handler tried to acquire
  // the same lock, it would spin forever waiting for code (us) that
  // can't run again until the handler returns.
  pushcli();
  if(holding(lk))
    panic("acquire");

  // The xchg is atomic: only one CPU can ever see it return the old
  // value 0 (lock free) and fall out of the loop with lk->locked now
  // set to 1 by that same instruction; everyone else keeps spinning.
  while(xchg(&lk->locked, 1) != 0)
    ;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen after the lock is acquired.
  __sync_synchronize();

  // Record info about lock acquisition for debugging.
  lk->cpu = mycpu();
  getcallerpcs(&lk, lk->pcs);
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->pcs[0] = 0;
  lk->cpu = 0;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other cores before the lock is released.
  // Both the C compiler and the hardware may re-order loads and
  // stores; __sync_synchronize() tells them both not to.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This can't be a plain C assignment: clearlock() (kernel/x86.asm) is a
  // real function call, which the compiler can't reorder or optimize
  // away, so the store is guaranteed to happen exactly here, after the
  // release fence above. A real OS would use C atomics here.
  clearlock(&lk->locked);

  popcli();
}

// Record the current call stack in pcs[] by following the %ebp chain.
// Relies on every function maintaining a standard stack frame - each
// %ebp points at the caller's saved %ebp, with the return address right
// above it - which is why the Makefile always builds with
// -fno-omit-frame-pointer. v is the address of a parameter or local
// variable of the function that called getcallerpcs; two words below
// it sit that function's saved %ebp and, above that, its caller's return
// address, which is where the walk starts.
void
getcallerpcs(void *v, uintp pcs[])
{
  uintp *ebp;
  int i;

  ebp = (uintp*)v - 2;
  for(i = 0; i < 10; i++){
    if(ebp == 0 || ebp < (uintp*)KERNBASE || ebp == (uintp*)-1)
      break;
    pcs[i] = ebp[1];       // saved %eip/%rip
    ebp = (uintp*)ebp[0];  // saved %ebp/%rbp
  }
  for(; i < 10; i++)
    pcs[i] = 0;
}

// Check whether this cpu is holding the lock.
int
holding(struct spinlock *lock)
{
  int r;
  pushcli();
  r = lock->locked && lock->cpu == mycpu();
  popcli();
  return r;
}


// Pushcli/popcli are like cli/sti except that they are matched:
// it takes two popcli to undo two pushcli.  Also, if interrupts
// are off, then pushcli, popcli leaves them off.

void
pushcli(void)
{
  int eflags;

  eflags = readeflags();
  cli();
  if(mycpu()->ncli == 0)
    mycpu()->intena = eflags & FL_IF;
  mycpu()->ncli += 1;
}

void
popcli(void)
{
  if(readeflags()&FL_IF)
    panic("popcli - interruptible");
  if(--mycpu()->ncli < 0)
    panic("popcli");
  if(mycpu()->ncli == 0 && mycpu()->intena)
    sti();
}

