// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

// NDIRECT is 11, not 12: dinode must divide BSIZE evenly (IPB, mkfs's
// inode-block layout), so adding a doubly-indirect pointer to addrs[]
// trades away one direct-block slot rather than growing the struct -
// addrs[NDIRECT+2] below is still 13 total slots, exactly what
// addrs[NDIRECT+1] was at the old NDIRECT==12 (dinode stays 64 bytes,
// IPB stays 8). Growing addrs[] by a slot instead (keeping NDIRECT==12)
// was tried first and rejected: 12+4*14=68 bytes doesn't divide 512,
// tripping mkfs.c's own BSIZE%sizeof(struct dinode)==0 assertion.
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
// NDINDIRECT: a doubly-indirect block - one block of NINDIRECT pointers,
// each itself pointing to a singly-indirect block of NINDIRECT data-block
// pointers (the classic xv6-large-files scheme). Added specifically
// because MAXFILE's old ~70KB (140-block) ceiling meant libc.so itself
// (needed for any coreutils command at all) couldn't grow past a
// handful of real coreutils utilities before hitting it - see the
// Makefile's own MUSL_LDSO_OBJS comment.
#define NDINDIRECT (NINDIRECT * NINDIRECT)
#define MAXFILE (NDIRECT + NINDIRECT + NDINDIRECT)

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEV only)
  short minor;          // Minor device number (T_DEV only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+2];   // Data block addresses (direct, singly-indirect, doubly-indirect)
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) (b/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

