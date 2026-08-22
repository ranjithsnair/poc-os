// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

// A free page needs no metadata of its own: kfree() writes this little
// linked-list node directly into the first bytes of the page being
// freed, so the free list costs no memory beyond the free pages
// themselves.
struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

// Per-physical-page reference count, indexed by pa/PGSIZE - the basis
// for copy-on-write fork (kernel/vm.c's copyuvm()/vm_handle_pagefault()).
// BSS-zeroed at boot: every entry starts at 0, which kfree() below
// treats as "never kalloc()'d before" (the boot-time freerange() case)
// rather than a real reference dropping to a negative count. Sized to
// cover every physical page below PHYSTOP - the only range kalloc()
// ever hands out (device memory like the real framebuffer, always
// >= PHYSTOP, is mapped directly and never touches this table - see
// kernel/vm.c's copyuvm() pa>=PHYSTOP branch).
static ushort pageref[PHYSTOP / PGSIZE];

// Guarded by kmem.lock (already held across every kalloc()/kfree() call
// below) rather than a second lock - refcounts only ever change from
// inside kalloc()/kfree()/kaddref(), so one lock covering all three is
// enough and avoids any lock-ordering question between two locks.
static uint
pageidx(uintp pa)
{
  return (uint)(pa / PGSIZE);
}

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using the boot-time page table
// kernel/entry.asm built (entrypml4/entrypdpt_low/entrypdpt_high/
// entrypd) to place just the pages that table maps on the free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uintp)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
//
// Copy-on-write fork (kernel/vm.c's copyuvm()) can leave one physical
// page mapped into more than one process's page table, each with its
// own kfree() call as it exits/execs/shrinks (kernel/vm.c's
// deallocuvm()) - so this only actually returns the page to the
// freelist once every such caller has dropped its share. A pageref[]
// entry that's still 0 means this exact page has never been through
// kalloc() since boot (freerange()'s own initial seeding of the free
// list, kinit1()/kinit2() below) - free it unconditionally, the same
// as before refcounting existed, rather than underflowing a count that
// was never incremented in the first place.
void
kfree(char *v)
{
  struct run *r;
  uint idx;
  int dofree;

  if((uintp)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  idx = pageidx(V2P(v));

  if(kmem.use_lock)
    acquire(&kmem.lock);
  if(pageref[idx] == 0)
    dofree = 1;
  else if(--pageref[idx] == 0)
    dofree = 1;
  else
    dofree = 0;
  if(dofree){
    // Fill with junk to catch dangling refs - only safe now that we
    // know no other mapping still shares this physical page.
    memset(v, 1, PGSIZE);
    r = (struct run*)v;
    r->next = kmem.freelist;
    kmem.freelist = r;
  }
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Add one reference to an already-kalloc()'d page - called by
// kernel/vm.c's copyuvm() when a fork shares a page COW instead of
// copying it, and by vm_handle_pagefault() when a COW fault resolves
// without needing a fresh copy.
void
kaddref(uintp pa)
{
  if(kmem.use_lock)
    acquire(&kmem.lock);
  pageref[pageidx(pa)]++;
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Current reference count of an already-kalloc()'d page - used by
// kernel/vm.c's vm_handle_pagefault() to tell "I'm the last owner,
// just reclaim this page in place" from "still shared, must copy".
int
kgetref(uintp pa)
{
  int n;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  n = pageref[pageidx(pa)];
  if(kmem.use_lock)
    release(&kmem.lock);
  return n;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    // A freshly-handed-out page always starts single-owner, regardless
    // of what it was last used for (kfree() only ever leaves a page on
    // the freelist once its refcount already reached 0) - set, not
    // increment.
    pageref[pageidx(V2P(r))] = 1;
  }
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}

