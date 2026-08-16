// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

// Mode bits kernel/fs.c's permcheck() checks want-arguments against -
// same numeric values as POSIX's S_IROTH/S_IWOTH/S_IXOTH (any of the
// three owner/group/other triplets), shifted into position by
// permcheck() itself before comparing.
#define PERM_R 4
#define PERM_W 2
#define PERM_X 1

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

// NDIRECT is 9, not 11: dinode must divide BSIZE evenly (IPB, mkfs's
// inode-block layout), so adding mode/uid/gid (multi-user support - see
// struct dinode below) trades away two more direct-block slots rather
// than growing the struct - the same precedent as the 12->11 change
// this comment used to describe when the doubly-indirect pointer was
// added: addrs[NDIRECT+2] below is still 11 total slots, exactly what
// addrs[NDIRECT+2] was at the old NDIRECT==11 minus the 2 slots traded
// away (dinode stays 64 bytes, IPB stays 8). Growing the struct past 64
// bytes instead was tried first and rejected: it would double IPB's
// disk overhead (128-byte dinode -> IPB 4, half as many inodes per
// block) for the sake of 8 bytes of identity fields.
#define NDIRECT 9
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
  short type;            // File type
  short major;           // Major device number (T_DEV only)
  short minor;           // Minor device number (T_DEV only)
  short nlink;           // Number of links to inode in file system
  ushort mode;           // Permission bits: low 9 bits rwxrwxrwx, bits
                          // 9-11 setuid/setgid/sticky (S_ISUID/S_ISGID/
                          // S_ISVTX) - same bit layout as musl's own
                          // <sys/stat.h>, so the fstat translation
                          // (musl/test/ldso_stubs.c) is a straight copy.
  ushort uid;             // Owner user ID
  ushort gid;             // Owner group ID
  ushort rsvd;            // Alignment filler, zeroed, unused for now
  uint size;             // Size of file (bytes)
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

