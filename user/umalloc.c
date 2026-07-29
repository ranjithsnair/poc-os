#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"

// Memory allocator by Kernighan and Ritchie,
// The C programming Language, 2nd ed.  Section 8.7.
//
// Free blocks form a circular, address-ordered singly-linked list (each
// block's header points to the next free block, and the last one's
// pointer wraps back to the first); freep is just wherever in that ring
// the last operation left off, not necessarily the "first" block in any
// meaningful sense. Every block, free or in-use, is prefixed by a Header
// recording its size in Header-sized units, which is how free() finds
// the header for a raw pointer (ap - 1) and how adjacent free blocks
// get merged back together (coalesced) below.

typedef long Align;

// size is in units of sizeof(Header), including this header itself;
// Align forces every block to be aligned suitably for any C type.
union header {
  struct {
    union header *ptr;
    uint size;
  } s;
  Align x;
};

typedef union header Header;

static Header base;
static Header *freep;

// Splices the block ap points at back into the free ring, at whatever
// position keeps the ring address-ordered, and merges it with either
// neighbor it turns out to be adjacent to (so freeing memory doesn't
// leave the heap fragmented into blocks smaller than they need to be).
void
free(void *ap)
{
  Header *bp, *p;

  bp = (Header*)ap - 1;
  // Find the free block p that bp belongs after: normally that means
  // walking until bp falls between p and p->s.ptr, but since the list
  // is circular, also stop if p is the highest-or-lowest-addressed
  // block in the ring and bp belongs at that wraparound point (above
  // the highest or below the lowest).
  for(p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if(p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;
  // Coalesce with the block above, if bp's end touches it exactly.
  if(bp + bp->s.size == p->s.ptr){
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else
    bp->s.ptr = p->s.ptr;
  // Coalesce with the block below, if its end touches bp exactly.
  if(p + p->s.size == bp){
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else
    p->s.ptr = bp;
  freep = p;
}

// Ask the kernel for more heap (via sbrk) when the free list has
// nothing big enough left, format it as one big free block, and hand
// it to free() to link into the ring (and coalesce with whatever's
// already there, if it happens to be adjacent).
static Header*
morecore(uint nu)
{
  char *p;
  Header *hp;

  // Round up to a whole page's worth of headers at a time, rather than
  // exactly nu, so a string of small mallocs doesn't turn into a string
  // of separate sbrk system calls.
  if(nu < 4096)
    nu = 4096;
  p = sbrk(nu * sizeof(Header));
  if(p == (char*)-1)
    return 0;
  hp = (Header*)p;
  hp->s.size = nu;
  free((void*)(hp + 1));
  return freep;
}

void*
malloc(uint nbytes)
{
  Header *p, *prevp;
  uint nunits;

  // +1 for this block's own header, rounded up to a whole number of
  // Header-sized units.
  nunits = (nbytes + sizeof(Header) - 1)/sizeof(Header) + 1;
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }
  // First-fit search around the free ring, starting from wherever the
  // last allocation or free left freep.
  for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
    if(p->s.size >= nunits){
      if(p->s.size == nunits)
        // Exact fit: unlink the whole block from the free ring.
        prevp->s.ptr = p->s.ptr;
      else {
        // Split: carve nunits off the end of this block and keep the
        // remainder (still at the same starting address) in the free
        // ring, so the returned block is the tail, not the head.
        p->s.size -= nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      return (void*)(p + 1);
    }
    // Back where we started a full loop around the ring with nothing
    // big enough found - ask the kernel for more memory.
    if(p == freep)
      if((p = morecore(nunits)) == 0)
        return 0;
  }
}
