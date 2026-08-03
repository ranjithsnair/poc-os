// mkfs: a host-side tool (built with the host's own gcc, not the poc
// cross-toolchain - see the Makefile) that builds an poc file system
// image from scratch and copies in the given files. Run once at build
// time to produce build/fs.img; poc itself never runs this code.

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat poc_stat  // avoid clash with host struct stat
#include "types.h"
#include "fs.h"
#include "stat.h"
#include "param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/(BSIZE*8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;
uint rootino;

// Directories created so far by ensure_dir() below, keyed by their
// full path relative to the image root (no leading/trailing '/') -
// e.g. "usr/lib". Small and linearly searched since a build only ever
// needs a handful of directories (unlike NINODES/freeinode, which
// track every inode mkfs allocates), not because paths in general are
// few - mkfs is a one-shot build-time tool, not something that has to
// scale.
#define MAXDIRTAB 32
struct dirtab { char path[DIRSIZ*4]; uint inum; } dirtab[MAXDIRTAB];
int ndirtab = 0;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
uint mkdirat(uint parentino, const char *name);
uint ensure_dir(const char *path);
void installfile(const char *imgpath, const char *hostpath);

// Convert to little-endian (x86's byte order) explicitly, byte by byte,
// rather than just writing x directly: this host tool might itself be
// compiled and run on a big-endian machine, but the fs.img it produces
// must always match what an x86 poc kernel expects on disk regardless.
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i;
  uint off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0){
    perror(argv[1]);
    exit(1);
  }

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  freeblock = nmeta;     // the first free block that we can allocate

  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  for(i = 2; i < argc; i++){
    // Two forms: a bare host path (e.g. build/_cat, the original and
    // still-default convention - root-placed, basename only, leading
    // "_" stripped so the build OS doesn't try to execute these in
    // place of real system binaries like rm/cat) or, if it contains a
    // ':', "imgpath:hostpath" (e.g. usr/lib/libc.so:build/libc.so) -
    // installed at that exact path, verbatim, creating any missing
    // parent directories along the way (see ensure_dir()). No
    // underscore-stripping for this form: the caller already spells
    // out the exact name it wants.
    char *arg = argv[i];
    char *colon = strchr(arg, ':');

    if(colon){
      *colon = 0;
      installfile(arg, colon + 1);
    } else {
      char *name = strrchr(arg, '/');
      name = name ? name + 1 : arg;
      if(name[0] == '_')
        ++name;
      installfile(name, arg);
    }
  }

  // fix size of root inode dir
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  balloc(freeblock);

  exit(0);
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(write(fsfd, buf, BSIZE) != BSIZE){
    perror("write");
    exit(1);
  }
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(read(fsfd, buf, BSIZE) != BSIZE){
    perror("read");
    exit(1);
  }
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BSIZE*8);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint dindirect[NINDIRECT];
  uint indirect2[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else if(fbn < NDIRECT + NINDIRECT){
      uint ibn = fbn - NDIRECT;
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[ibn] == 0){
        indirect[ibn] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[ibn]);
    } else {
      // Doubly-indirect range - mirrors kernel/fs.c's bmap() exactly
      // (see include/fs.h's NDINDIRECT/MAXFILE comment): din.addrs
      // [NDIRECT+1] holds NINDIRECT pointers to indirect blocks, each
      // of which holds NINDIRECT data-block pointers in turn.
      uint bn2 = fbn - NDIRECT - NINDIRECT;
      uint idx1 = bn2 / NINDIRECT;
      uint idx2 = bn2 % NINDIRECT;

      if(xint(din.addrs[NDIRECT+1]) == 0){
        din.addrs[NDIRECT+1] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT+1]), (char*)dindirect);
      if(dindirect[idx1] == 0){
        dindirect[idx1] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT+1]), (char*)dindirect);
      }
      rsect(xint(dindirect[idx1]), (char*)indirect2);
      if(indirect2[idx2] == 0){
        indirect2[idx2] = xint(freeblock++);
        wsect(xint(dindirect[idx1]), (char*)indirect2);
      }
      x = xint(indirect2[idx2]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}

// Create a directory named name inside the already-created directory
// parentino, with "." and ".." entries, and link it into parentino
// under that name - i.e. exactly what kernel/sysfile.c's create()
// does for a T_DIR, including its parentino->nlink++ for the new
// directory's ".." (see that function's own comment on why "." does
// *not* also bump the new directory's own nlink), so a directory
// mkfs creates is indistinguishable on disk from one a real mkdir()
// syscall would have created at runtime.
uint
mkdirat(uint parentino, const char *name)
{
  uint inum = ialloc(T_DIR);
  struct dirent de;
  struct dinode din;

  bzero(&de, sizeof(de));
  de.inum = xshort(inum);
  strcpy(de.name, ".");
  iappend(inum, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(parentino);
  strcpy(de.name, "..");
  iappend(inum, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(inum);
  strncpy(de.name, name, DIRSIZ);
  iappend(parentino, &de, sizeof(de));

  rinode(parentino, &din);
  din.nlink = xshort(xshort(din.nlink) + 1);
  winode(parentino, &din);

  return inum;
}

// Return the inode number of the directory at path (relative to the
// image root, no leading/trailing '/'), creating it - and any missing
// parent directories - if it doesn't already exist. path == "" (or
// NULL) means the root directory itself.
uint
ensure_dir(const char *path)
{
  int i;
  char parent[DIRSIZ*8];
  const char *name;
  char *slash;
  uint parentino, inum;

  if(path == 0 || path[0] == 0)
    return rootino;

  for(i = 0; i < ndirtab; i++)
    if(strcmp(dirtab[i].path, path) == 0)
      return dirtab[i].inum;

  strncpy(parent, path, sizeof(parent)-1);
  parent[sizeof(parent)-1] = 0;
  slash = strrchr(parent, '/');
  if(slash){
    *slash = 0;
    // parent (up to slash) is an identical-prefix copy of path, so
    // slash's offset within it is also name's starting offset within
    // the original, untruncated path.
    name = path + (slash - parent) + 1;
    parentino = ensure_dir(parent);
  } else {
    name = path;
    parentino = rootino;
  }

  if(ndirtab >= MAXDIRTAB){
    fprintf(stderr, "mkfs: too many directories (MAXDIRTAB=%d)\n", MAXDIRTAB);
    exit(1);
  }
  inum = mkdirat(parentino, name);
  strncpy(dirtab[ndirtab].path, path, sizeof(dirtab[ndirtab].path)-1);
  dirtab[ndirtab].path[sizeof(dirtab[ndirtab].path)-1] = 0;
  dirtab[ndirtab].inum = inum;
  ndirtab++;
  return inum;
}

// Install the file at hostpath into the image at imgpath (relative to
// the image root), creating any missing parent directories along the
// way - see main()'s argv loop for the two ways a caller reaches this
// (bare host path vs "imgpath:hostpath").
void
installfile(const char *imgpath, const char *hostpath)
{
  int fd, cc;
  uint inum, parentino;
  char dir[DIRSIZ*8];
  const char *name;
  char *slash;
  struct dirent de;
  char buf[BSIZE];

  strncpy(dir, imgpath, sizeof(dir)-1);
  dir[sizeof(dir)-1] = 0;
  slash = strrchr(dir, '/');
  if(slash){
    *slash = 0;
    name = imgpath + (slash - dir) + 1;
    parentino = ensure_dir(dir);
  } else {
    name = imgpath;
    parentino = rootino;
  }

  if((fd = open(hostpath, 0)) < 0){
    perror(hostpath);
    exit(1);
  }

  inum = ialloc(T_FILE);

  bzero(&de, sizeof(de));
  de.inum = xshort(inum);
  strncpy(de.name, name, DIRSIZ);
  iappend(parentino, &de, sizeof(de));

  while((cc = read(fd, buf, sizeof(buf))) > 0)
    iappend(inum, buf, cc);

  close(fd);
}
