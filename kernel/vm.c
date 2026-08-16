// Virtual memory: building and tearing down four-level page tables
// (see walkpgdir), the fixed kernel mapping shared by every process
// (see kmap[] below), and the user-memory helpers (allocuvm/
// deallocuvm/copyuvm/copyout/...) used by exec(), fork(), sbrk(), and
// system calls that read or write user-supplied pointers. The
// user-memory helpers only ever call through walkpgdir/mappages, never
// touch a page table level directly.

#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors, and load the (temporary,
// bootstrap) IDT-less GDT built by entry.asm - run once on entry on
// each CPU. Segmentation is flat/ignored for data access in long mode,
// so there's no separate user-vs-kernel data segment distinction that
// matters; SEG_UCODE/SEG_UDATA still exist so user mode has its own
// DPL=3 code segment to run with.
void
seginit(void)
{
  struct cpu *c;

  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEGL(STA_X|STA_R, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEGL(STA_X|STA_R, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Descend one level of a multi-level page table: return the next-level
// table that table[idx] refers to, allocating and zeroing a fresh page
// for it first if none exists yet and alloc is set.
static pte_t *
walknext(pte_t *table, uint idx, int alloc)
{
  pte_t *pgtab;

  if(table[idx] & PTE_P)
    return (pte_t*)P2V(PTE_ADDR(table[idx]));
  if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
    return 0;
  memset(pgtab, 0, PGSIZE);
  // The permissions here are overly generous, but they can be further
  // restricted by the permissions in the leaf page table entry, if
  // necessary.
  table[idx] = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  return pgtab;
}

// Return the address of the PD entry for va (walking PML4->PDPT->PD,
// allocating missing intermediate pages if alloc), without descending
// to a PT. Used both by walkpgdir below (one more level, to a 4KB PTE)
// and directly by mappages for a 2MB PS-bit leaf, which stops here.
static pte_t *
walkpd(pde_t *pgdir, const void *va, int alloc)
{
  pte_t *pdpt, *pd;

  if((pdpt = walknext((pte_t*)pgdir, PML4X(va), alloc)) == 0)
    return 0;
  if((pd = walknext(pdpt, PDPTX(va), alloc)) == 0)
    return 0;
  return &pd[PDX(va)];
}

// Return the address of the PTE in page table pgdir (really its PML4)
// that corresponds to virtual address va, walking all four levels and
// allocating any missing intermediate table page along the way (if
// alloc).
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pte_t *pde, *pt;

  if((pde = walkpd(pgdir, va, alloc)) == 0)
    return 0;
  if(*pde & PTE_P){
    pt = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pt = (pte_t*)kalloc()) == 0)
      return 0;
    memset(pt, 0, PGSIZE);
    *pde = V2P(pt) | PTE_P | PTE_W | PTE_U;
  }
  return &pt[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not be
// page-aligned.
//
// Uses a 2MB (PS-bit) PD-level leaf instead of a 4KB PTE wherever va,
// pa, and the remaining range are all 2MB-aligned, needing no PT page
// at all for that chunk - falling back to 4KB pages for any unaligned
// leading/trailing remainder. This matters: setupkvm() below installs
// kmap[]'s ~224MB kernel-memory range and ~32MB DEVSPACE range in
// *every* process's page table, so without it each fork() would cost
// on the order of a hundred extra kalloc()'d PT pages just for those
// two entries alone - with NPROC(param.h)-scale fork/exit churn, that
// exhausts physical memory long before anything else does. (allocuvm's
// user-memory calls are always exactly PGSIZE at a time, so they never
// qualify for the 2MB path regardless - this only ever fires for the
// large, naturally-aligned kernel ranges.)
static int
mappages(pde_t *pgdir, void *va, uintp size, uintp pa, int perm)
{
  char *a, *last, *end;
  pte_t *pde, *pte;

  a = (char*)PGROUNDDOWN((uintp)va);
  last = (char*)PGROUNDDOWN(((uintp)va) + size - 1);
  end = (char*)((uintp)va + size);
  for(;;){
    if((uintp)a % PGSIZE2M == 0 && pa % PGSIZE2M == 0 &&
       (uintp)a + PGSIZE2M <= (uintp)end){
      if((pde = walkpd(pgdir, a, 1)) == 0)
        return -1;
      if(*pde & PTE_P)
        panic("remap");
      *pde = pa | perm | PTE_P | PTE_PS;
      a += PGSIZE2M;
      pa += PGSIZE2M;
      if((uintp)a > (uintp)last)
        break;
      continue;
    }
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uintp phys_start;
  uintp phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 // "up to the top of 32-bit physical address space" - uintp doesn't
 // wrap at 32 bits, so this end has to be written out explicitly
 // rather than relying on a phys_end=0/wraparound trick.
 { (void*)DEVSPACE, DEVSPACE,      0x100000000, PTE_W}, // more devices
};

// Map [va, va+size) in a *user* process's own page table directly to
// physical range [pa, pa+size) - a thin exported wrapper around the
// file-static mappages() above, the same idea as kmapphys() below for
// the kernel's own page table. The one caller today is kernel/
// sysproc.c's sys_mmap() FRAMEBUFFER path: the one case mmap() needs
// to hand a process a specific physical address (real VRAM) it doesn't
// otherwise control, rather than a fresh kalloc()'d page or a file's
// own content.
int
mapuvm_phys(pde_t *pgdir, uintp va, uintp size, uintp pa, int perm)
{
  return mappages(pgdir, (void*)va, size, pa, perm);
}

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  // No PHYSTOP-vs-DEVSPACE overlap check here: under this kernel's
  // canonical-high KERNBASE, P2V(PHYSTOP) and DEVSPACE are separated
  // by most of the 64-bit address space and can never collide (unlike
  // a modest 32-bit-style KERNBASE offset, where P2V(PHYSTOP) could
  // plausibly grow into DEVSPACE's range).
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  // The TSS descriptor is 16 bytes (two gdt[] slots) in long mode -
  // see struct segdesc64 and SEGTSS in mmu.h.
  *(struct segdesc64*)&mycpu()->gdt[SEG_TSS] =
    SEGTSS(STS_T64A, (uintp)&mycpu()->ts, sizeof(mycpu()->ts)-1, 0);
  mycpu()->ts.rsp0 = (uintp)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in rflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Free a page table and all the physical memory pages in the user
// part - and every intermediate PML4/PDPT/PD/PT page this specific
// pgdir owns, including the ones backing kmap[]'s kernel-shared
// mappings (setupkvm() rebuilds them fresh, via kalloc(), for every
// process's pgdir - see the comment on mappages' 2MB-page path - so
// they're this pgdir's own to free, not shared with any other
// process's). That means walking every one of the 512 PML4 slots, not
// just the low ones user memory and DEVSPACE populate: KERNBASE's own
// slot (PML4X(KERNBASE)=511) holds the "I/O space"/"kern text+rodata"/
// "kern data+memory" kmap[] entries.
void
freevm(pde_t *pgdir)
{
  uint i, j, k;
  pte_t *pdpt, *pd, *pt;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  // Bounded by DEVSPACE, not KERNBASE: DEVSPACE (0xFE000000, see
  // memlayout.h) is an *identity* mapping - kmap[]'s virt address for
  // it equals its own phys_start, unlike every other kmap[] entry,
  // which are KERNBASE-relative - and, numerically, it's low enough to
  // land in the same PML4 slot as ordinary user addresses (PML4X of
  // anything under 512GB is 0), not the high slot KERNBASE-relative
  // mappings use. deallocuvm() unconditionally assumes every present
  // leaf PTE it finds is KERNBASE-relative and P2V()-translates
  // accordingly; walking as far as raw KERNBASE would run it straight
  // into DEVSPACE's identity-mapped PTEs and hand kfree() a bogus,
  // wrapped-around "virtual" address computed from what's actually a
  // physical one.
  deallocuvm(pgdir, DEVSPACE, 0);

  for(i = 0; i < NPML4ENTRIES; i++){
    if(!(pgdir[i] & PTE_P))
      continue;
    pdpt = (pte_t*)P2V(PTE_ADDR(pgdir[i]));
    for(j = 0; j < NPDPTENTRIES; j++){
      if(!(pdpt[j] & PTE_P))
        continue;
      pd = (pte_t*)P2V(PTE_ADDR(pdpt[j]));
      for(k = 0; k < NPDENTRIES; k++){
        if(!(pd[k] & PTE_P))
          continue;
        // A 2MB PS-bit leaf (see mappages) - the physical memory it
        // describes isn't a kalloc()'d PT page, so there's nothing to
        // free at this level for it.
        if(pd[k] & PTE_PS)
          continue;
        pt = (pte_t*)P2V(PTE_ADDR(pd[k]));
        kfree((char*)pt);
      }
      kfree((char*)pd);
    }
    kfree((char*)pdpt);
  }
  kfree((char*)pgdir);
}

// Deallocate user pages to bring the process size from oldsz to
// newsz. Doesn't bother skipping over whole unmapped page-table
// regions in one jump - it just walks a page at a time, which costs a
// few extra (cheap) walkpgdir calls on a mostly-unmapped range but
// stays correct with no jump-shortcut logic to keep in sync with the
// real (4-level) page table structure.
int
deallocuvm(pde_t *pgdir, uintp oldsz, uintp newsz)
{
  pte_t *pte;
  uintp a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(pte && (*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      // pa >= PHYSTOP means this PTE was never a kalloc()'d page in
      // the first place - the framebuffer's real physical VRAM,
      // mapped in by kernel/sysproc.c's sys_mmap() FRAMEBUFFER path
      // (kernel/vm.c's mapuvm_phys()), is the one thing that lands
      // here today. kfree() itself panics on any address >= PHYSTOP
      // (kernel/kalloc.c) - calling it on device memory that was never
      // part of the free pool would either corrupt that pool or hit
      // that exact panic the moment any process holding such a
      // mapping exits/execs/shrinks. Just drop the mapping; there is
      // nothing to return to the allocator.
      if(pa < PHYSTOP){
        char *v = P2V(pa);
        kfree(v);
      }
      *pte = 0;
    }
  }
  return newsz;
}


// Make sure [pa, pa+size) is readable/writable at its own P2V(pa)
// virtual address in kpgdir, leaving any page that's already mapped
// there alone (unlike mappages(), which panics on a remap - callers
// like kernel/acpi.c's table walk map the same table's address twice,
// once by a conservative guess and once by its real length once
// that's known, and the two can overlap).
//
// Needed because kmap[]'s "kern data+memory" entry (see kmap[] above)
// only identity-maps physical memory up to PHYSTOP: real hardware and
// QEMU alike are free to place ACPI's tables anywhere in RAM, and on a
// large-enough -m size that can land above PHYSTOP even though
// PHYSTOP itself is an artificial cap this kernel imposes, not a
// description of how much RAM actually exists (see PHYSTOP's own
// comment in memlayout.h).
void
kmapphys(uintp pa, uintp size)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uintp)P2V(pa));
  last = (char*)PGROUNDDOWN((uintp)P2V(pa) + size - 1);
  for(;; a += PGSIZE){
    if((pte = walkpgdir(kpgdir, a, 1)) == 0)
      panic("kmapphys: out of memory");
    if(!(*pte & PTE_P))
      *pte = V2P(a) | PTE_W | PTE_P;
    if(a == last)
      break;
  }
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir. The pages from PGROUNDDOWN(addr)
// to addr+sz must already be mapped - addr itself need not be page-
// aligned (every existing caller's addr always has been, so this is a
// strict generalization: with addr page-aligned, off below is always
// 0 and boundary always PGSIZE, so the loop is byte-for-byte what it
// always was). A non-page-aligned addr is exactly what a shared
// object's second-and-later PT_LOAD segment usually has (see
// execve()'s interpreter-loading loop in kernel/exec.c): ELF only
// requires p_vaddr and p_offset to be congruent mod the segment's
// alignment, not that p_vaddr itself lands on a page boundary, and
// two consecutive PT_LOAD segments legitimately share the one page
// straddling their boundary.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uintp i, pa, n, off, boundary;
  pte_t *pte;

  for(i = 0; i < sz; i += n){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    off = ((uintp)addr + i) % PGSIZE;
    boundary = PGSIZE - off;
    n = (sz - i < boundary) ? sz - i : boundary;
    if(readi(ip, P2V(pa) + off, offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Zero out len bytes starting at addr, exactly like loaduvm above but
// filling with zeros instead of reading from an inode - the fd == -1
// (anonymous) counterpart to loaduvm, for sys_mmap()'s MAP_FIXED
// overlay path (kernel/sysproc.c): the range must already be mapped
// (typically by an earlier growproc()/allocuvm() reservation, which
// zero-fills on its own - this is for reusing that same reservation a
// second time, e.g. because a shared library's segments no longer
// cover a stale byte range a previous overlay of the same reservation
// left non-zero).
void
uvmzero(pde_t *pgdir, uintp addr, uintp len)
{
  uintp i, pa, n;
  pte_t *pte;

  if(addr % PGSIZE != 0)
    panic("uvmzero: addr must be page aligned");
  for(i = 0; i < len; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (char*)(addr+i), 0)) == 0)
      panic("uvmzero: address should exist");
    pa = PTE_ADDR(*pte);
    n = (len - i < PGSIZE) ? len - i : PGSIZE;
    memset(P2V(pa), 0, n);
  }
}

// Set or clear PTE_W on every already-mapped page in [addr, addr+len)
// (both must be page-aligned; callers round). Leaves PTE_P/PTE_U
// alone - poc-os's page tables track no NX-style execute-disable bit
// at all, so this is the only permission bit mprotect()/mmap() can
// meaningfully change. See sys_mprotect() and sys_mmap()'s MAP_FIXED
// path in kernel/sysproc.c.
void
uvmsetperm(pde_t *pgdir, uintp addr, uintp len, int writable)
{
  uintp a;
  pte_t *pte;

  for(a = addr; a < addr + len; a += PGSIZE){
    if((pte = walkpgdir(pgdir, (char*)a, 0)) == 0)
      panic("uvmsetperm: address should exist");
    if(writable)
      *pte |= PTE_W;
    else
      *pte &= ~PTE_W;
  }
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uintp oldsz, uintp newsz)
{
  char *mem;
  uintp a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uintp sz)
{
  pde_t *d;
  pte_t *pte;
  uintp pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uintp va, void *p, uintp len)
{
  char *buf, *pa0;
  uintp n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uintp)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
