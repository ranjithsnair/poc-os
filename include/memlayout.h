// Memory layout

#define EXTMEM  0x100000            // Start of extended memory
#define PHYSTOP 0xE000000           // Top physical memory
#define DEVSPACE 0xFE000000         // Other devices are at high addresses

// Key addresses for address space layout (see kmap in vm.c for layout)
//
// On the 64-bit build, KERNBASE is a canonical higher-half address (top
// -2GB, the same shape Linux uses) rather than the 32-bit build's 2GB
// offset - PHYSTOP is modest (~224MB, above) so it, plus DEVSPACE, fits
// comfortably in the 2GB above KERNBASE without needing a separate
// physical direct-map region: V2P/P2V stay a plain offset by KERNBASE,
// exactly like the 32-bit build.
#ifdef X64
#define KERNBASE 0xFFFFFFFF80000000
#else
#define KERNBASE 0x80000000         // First kernel virtual address
#endif
#define KERNLINK (KERNBASE+EXTMEM)  // Address where kernel is linked

#define V2P(a) (((uintp) (a)) - KERNBASE)
#define P2V(a) ((void *)(((uintp) (a)) + KERNBASE))

#define V2P_WO(x) ((x) - KERNBASE)    // same as V2P, but without casts
#define P2V_WO(x) ((x) + KERNBASE)    // same as P2V, but without casts
