#define NPROC        64  // maximum number of processes
#define KSTACKSIZE 4096  // size of per-process kernel stack
// USTACKPAGES: usable pages of user stack exec.c allocates (below a
// single guard page - see its own comment). A dynamically-linked
// program's real gnulib call chains run far deeper, with far bigger
// per-frame locals (vsnprintf's own on-stack ~256-byte buffer, times
// however many of nstrftime()/human_readable()/quote_name_buf()/...
// are nested when ls -l formats one line), than any of poc-os's own
// native, statically-linked user/*.c programs ever needed - 1 page
// (the original xv6 value) turned out to be too little the first
// time a real coreutils utility's deepest call chain (ls -l, of every
// utility shipped so far) actually got exercised: `rep stosl`
// zeroing vsnprintf's own local buffer faulted on a stack page past
// the single one mapped, well before any genuinely-unbounded
// recursion or runaway allocation was involved.
#define USTACKPAGES  16
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXENVP      32  // max execve environment variables
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
// mkfs/mkfs.c's balloc() writes the whole free-block bitmap as a
// single BSIZE-byte sector (one wsect() call, no indirection) - a
// hard ceiling of BSIZE*8 = 4096 blocks (2MB) on FSSIZE, not just a
// default; raising this past 4095 needs balloc() itself extended to
// span multiple bitmap sectors first. 4000 (2MB, up from the
// original 1000/500KB) is comfortably under that ceiling while
// giving real room to grow: as of this comment, UPROGS plus
// usr/lib/libc.so plus usr/bin/cat alone (see the Makefile) already
// used 884 of the original 1000 blocks, leaving usertests.c's own
// bigfile test (writetest1(), which deliberately writes a
// MAXFILE-sized ~70KB/140-block file to exercise indirect-block
// handling) no room to run without hitting balloc()'s "out of
// blocks" panic - confirmed by hitting exactly that panic once real
// coreutils utilities pushed usage that high.
#define FSSIZE       4000  // size of file system in blocks

