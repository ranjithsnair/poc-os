// Sleeping locks
//
// Unlike a spinlock, a sleeplock can be held across operations that take
// a while (such as file system I/O): a process that can't acquire it
// calls sleep() to give up the CPU instead of spinning, and is woken up
// again once the holder releases it. Every sleeplock is itself guarded
// by a small internal spinlock (lk->lk) that only ever protects the
// "locked"/"pid" fields for the brief moment they're inspected or
// updated - it is not held across the sleep() call itself.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"

void
initsleeplock(struct sleeplock *lk, char *name)
{
  initlock(&lk->lk, "sleep lock");
  lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
}

void
acquiresleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  // sleep() atomically releases lk->lk and parks this process; whoever
  // wakes us up (releasesleep, below) has lk->lk free for us to
  // re-acquire, so we always come back from sleep() holding it again.
  // The while loop re-checks lk->locked in case of a spurious wakeup or
  // another process winning the race to grab it first.
  while (lk->locked) {
    sleep(lk, &lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);
}

void
releasesleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
}

// Is the current process the one holding lk? Used by callers to assert
// they really do hold a lock before touching the data it protects.
int
holdingsleep(struct sleeplock *lk)
{
  int r;

  acquire(&lk->lk);
  r = lk->locked && (lk->pid == myproc()->pid);
  release(&lk->lk);
  return r;
}



