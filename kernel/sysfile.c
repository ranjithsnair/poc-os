//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// A few real errno values, needed only where a caller's control flow
// genuinely depends on distinguishing one error from another (see
// sys_lseek()'s and sys_open()'s own comments) - every other error
// return in this file, as in the rest of this kernel, is still a bare
// -1 (musl's __syscall_ret turns that into errno==EPERM, regardless of
// what actually went wrong; fine as long as nothing needs to tell them
// apart).
#define ENOENT 2
#define EINVAL 22

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
// Not static: kernel/sysnet.c/shm.c/epoll.c (sockets/shm/epoll
// syscalls, GUI roadmap phase 3) reuse this and fdalloc() below rather
// than duplicating them - declared in defs.h.
int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

// (fd, iov, iovcnt): see include/syscall.h. struct iovec is
// {void *iov_base; size_t iov_len;}, 16 bytes on the 64-bit build;
// argptr isn't used here since iov_base/iov_len live inside a
// caller-provided array rather than being syscall arguments
// themselves, so each field is fetched (and, like argptr, bounds-
// checked against curproc->sz) by hand instead.
int
sys_writev(void)
{
  struct file *f;
  int iovp, iovcnt;
  int i, total, base, len, n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(1, &iovp) < 0 || argint(2, &iovcnt) < 0)
    return -1;
  if(iovcnt < 0 || iovcnt > 16)
    return -1;

  total = 0;
  for(i = 0; i < iovcnt; i++){
    if(fetchint(iovp + 16*i, &base) < 0 || fetchint(iovp + 16*i + 8, &len) < 0)
      return total > 0 ? total : -1;
    if(len == 0)
      continue;
    if(len < 0 || (uint)base >= myproc()->sz || (uint)base+len > myproc()->sz)
      return total > 0 ? total : -1;
    p = (char*)(uintp)(uint)base;
    if((n = filewrite(f, p, len)) < 0)
      return total > 0 ? total : -1;
    total += n;
    if(n < len)
      break;  // short write - stop, like a real writev would
  }
  return total;
}

// (fd, iov, iovcnt): see include/syscall.h. Read-side mirror of
// sys_writev() above - same argument fetching, same per-iovec loop,
// fileread() instead of filewrite().
int
sys_readv(void)
{
  struct file *f;
  int iovp, iovcnt;
  int i, total, base, len, n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(1, &iovp) < 0 || argint(2, &iovcnt) < 0)
    return -1;
  if(iovcnt < 0 || iovcnt > 16)
    return -1;

  total = 0;
  for(i = 0; i < iovcnt; i++){
    if(fetchint(iovp + 16*i, &base) < 0 || fetchint(iovp + 16*i + 8, &len) < 0)
      return total > 0 ? total : -1;
    if(len == 0)
      continue;
    if(len < 0 || (uint)base >= myproc()->sz || (uint)base+len > myproc()->sz)
      return total > 0 ? total : -1;
    p = (char*)(uintp)(uint)base;
    if((n = fileread(f, p, len)) < 0)
      return total > 0 ? total : -1;
    total += n;
    if(n < len)
      break;  // short read - stop, like a real readv would
  }
  return total;
}

// (fd, buf, count): Linux getdents64 ABI - see include/syscall.h and
// musl/src/dirent/readdir.c, the only caller that matters: it casts
// the raw bytes this writes directly to struct dirent* and reads
// d_reclen/d_ino/d_name straight out of them, no further translation.
// Translates poc-os's own on-disk directory format (include/fs.h's
// struct dirent - a flat array of fixed-size {inum, name[DIRSIZ]}
// slots, some empty/deleted with inum 0) into that ABI's variable-
// length records, resuming from f->off exactly like sys_read/sys_readv
// already do for regular files.
struct linux_dirent64 {
  uint64 d_ino;
  uint64 d_off;
  ushort d_reclen;
  uchar d_type;
  char d_name[DIRSIZ+1];
} __attribute__((packed));

int
sys_getdents(void)
{
  struct file *f;
  char *buf;
  int n, total, reclen;
  struct dirent de;
  struct linux_dirent64 ld;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &buf, n) < 0)
    return -1;
  if(f->type != FD_INODE || f->ip->type != T_DIR)
    return -1;

  ilock(f->ip);
  total = 0;
  while(f->off + sizeof(de) <= f->ip->size){
    if(readi(f->ip, (char*)&de, f->off, sizeof(de)) != sizeof(de))
      panic("getdents: readi");
    if(de.inum == 0){
      // deleted/never-used slot (see sys_unlink's writei of a zeroed
      // de) - not a real entry, skip without handing it to the caller.
      f->off += sizeof(de);
      continue;
    }
    memset(&ld, 0, sizeof(ld));
    ld.d_ino = de.inum;
    memmove(ld.d_name, de.name, DIRSIZ);
    ld.d_name[DIRSIZ] = 0;  // de.name may fill all DIRSIZ bytes with no NUL
    // Round up to keep the next record's d_ino/d_off naturally aligned
    // (x86_64 tolerates unaligned access, but nothing here relies on
    // that, matching what a real getdents64 buffer looks like).
    reclen = (__builtin_offsetof(struct linux_dirent64, d_name) + strlen(ld.d_name) + 1 + 7) & ~7;
    if(total + reclen > n)
      break;  // caller's buffer is full - retry from here next call
    f->off += sizeof(de);
    ld.d_off = f->off;
    ld.d_reclen = reclen;
    memmove(buf + total, &ld, reclen);
    total += reclen;
  }
  iunlock(f->ip);
  return total;
}

// (fd, offset, whence): see include/syscall.h.
int
sys_lseek(void)
{
  struct file *f;
  int offset, whence;
  uint newoff;

  if(argfd(0, 0, &f) < 0 || argint(1, &offset) < 0 || argint(2, &whence) < 0)
    return -1;
  if(f->type != FD_INODE)
    return -1;

  switch(whence){
  case 0:  // SEEK_SET
    if(offset < 0)
      return -1;
    newoff = (uint)offset;
    break;
  case 1:  // SEEK_CUR
    if((int)f->off + offset < 0)
      return -1;
    newoff = f->off + offset;
    break;
  case 2:  // SEEK_END
    if((int)f->ip->size + offset < 0)
      return -1;
    newoff = f->ip->size + offset;
    break;
  default:
    // Real Linux whence values this kernel has no case for (SEEK_DATA/
    // SEEK_HOLE, e.g. - poc-os has no sparse files, see __fstat()'s
    // st_blocks comment) need a real -EINVAL, not the bare -1 every
    // other error in this file returns: cp's infer_scantype() (real
    // gnulib, coreutils/src/copy.c) checks errno==EINVAL specifically
    // to fall back gracefully when a lseek() whence isn't supported,
    // rather than treating it as a real error worth aborting the copy
    // over.
    return -EINVAL;
  }
  f->off = newoff;
  return newoff;
}

// (fd, buf, count, offset): see include/syscall.h.
int
sys_pread(void)
{
  struct file *f;
  char *p;
  int n, offset;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0 ||
     argint(3, &offset) < 0)
    return -1;
  if(f->type != FD_INODE || f->readable == 0 || offset < 0)
    return -1;
  ilock(f->ip);
  n = readi(f->ip, p, (uint)offset, n);
  iunlock(f->ip);
  return n;
}

// (fd, cmd, arg): see include/syscall.h.
int
sys_fcntl(void)
{
  return 0;
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(permcheck(dp, myproc()->euid, myproc()->egid, PERM_W) < 0){
    iunlockput(dp);
    goto bad;
  }
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  // Unlink permission is about the *directory*, not the file being
  // removed - standard Unix semantics.
  if(permcheck(dp, myproc()->euid, myproc()->egid, PERM_W) < 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

// (old, new): a real rename(2) - see coreutils_shims.c's own rename()
// comment for why this exists: its previous link()+unlink() emulation
// (the standard userspace fallback on a filesystem with hard links
// but no native rename) could never move a directory, since
// sys_link() correctly refuses to hard-link one - hard-linking a
// directory would let it have two distinct parents, corrupting the
// single-parent tree every ".." entry and nlink count assumes. A real
// rename() sidesteps that entirely: it moves the *directory entry*
// (rewrite an on-disk dirent's inum in the new parent, zero it in the
// old one), never touching ip->nlink for the moved inode itself, so
// sys_link()'s restriction never comes into play. Only ever locks one
// inode/directory at a time (unlike a real rename() that would lock
// both directories for true atomicity against a concurrent reverse
// rename) - deliberately, to make deadlock structurally impossible
// rather than relying on a lock-ordering discipline every future
// caller would also have to get right; the whole move still happens
// inside one begin_op()/end_op() transaction, so a crash mid-rename
// still leaves a consistent on-disk state either fully before or
// fully after, just not perfectly atomic against another rename
// racing on the same two directories.
static int
samefile(struct inode *a, struct inode *b)
{
  return a->dev == b->dev && a->inum == b->inum;
}

int
sys_rename(void)
{
  char oldname[DIRSIZ], newname[DIRSIZ];
  char *oldpath, *newpath;
  struct inode *olddp, *newdp, *ip, *xip;
  uint oldoff, newoff;
  int ip_is_dir;

  if(argstr(0, &oldpath) < 0 || argstr(1, &newpath) < 0)
    return -1;

  begin_op();

  if((olddp = nameiparent(oldpath, oldname)) == 0){
    end_op();
    return -1;
  }
  if(namecmp(oldname, ".") == 0 || namecmp(oldname, "..") == 0){
    iput(olddp);
    end_op();
    return -1;
  }

  ilock(olddp);
  if(permcheck(olddp, myproc()->euid, myproc()->egid, PERM_W) < 0){
    iunlockput(olddp);
    end_op();
    return -1;
  }
  ip = dirlookup(olddp, oldname, &oldoff);
  iunlock(olddp);
  if(ip == 0){
    iput(olddp);
    end_op();
    return -1;
  }

  if((newdp = nameiparent(newpath, newname)) == 0){
    iput(olddp);
    iput(ip);
    end_op();
    return -1;
  }
  if(newdp->dev != ip->dev ||
     namecmp(newname, ".") == 0 || namecmp(newname, "..") == 0){
    iput(olddp);
    iput(newdp);
    iput(ip);
    end_op();
    return -1;
  }

  ilock(ip);
  ip_is_dir = (ip->type == T_DIR);
  iunlock(ip);

  // Renaming X to (a path inside) itself would rewrite X's own ".."
  // out from under this very walk, or leave some ancestor of newdp
  // unreachable from the root once X is unlinked from oldname below -
  // walk from newdp back up to the root via ".." (one directory
  // locked at a time, same reasoning as this function's own top
  // comment) and refuse if X itself is on that path.
  if(ip_is_dir){
    struct inode *walk = idup(newdp);
    int cycle = samefile(walk, ip);
    while(!cycle && walk->inum != ROOTINO){
      ilock(walk);
      struct inode *parent = dirlookup(walk, "..", 0);
      iunlockput(walk);
      if(parent == 0)
        break;
      walk = parent;
      cycle = samefile(walk, ip);
    }
    iput(walk);
    if(cycle){
      iput(olddp);
      iput(newdp);
      iput(ip);
      end_op();
      return -1;
    }
  }

  ilock(newdp);
  if(permcheck(newdp, myproc()->euid, myproc()->egid, PERM_W) < 0){
    iunlockput(newdp);
    iput(olddp);
    iput(ip);
    end_op();
    return -1;
  }
  xip = dirlookup(newdp, newname, &newoff);
  if(xip != 0){
    if(samefile(xip, ip)){
      // Renaming a path onto itself: nothing to do, not an error.
      iput(xip);
      iunlockput(newdp);
      iput(olddp);
      iput(ip);
      end_op();
      return 0;
    }
    ilock(xip);
    if(ip_is_dir != (xip->type == T_DIR) ||
       (xip->type == T_DIR && !isdirempty(xip))){
      iunlockput(xip);
      iunlockput(newdp);
      iput(olddp);
      iput(ip);
      end_op();
      return -1;
    }
    // Replace: drop the destination's own link the same way
    // sys_unlink() does (nlink--, and the parent's own nlink-- if it
    // was a - necessarily empty, just checked above - directory).
    if(xip->type == T_DIR){
      newdp->nlink--;
      iupdate(newdp);
    }
    xip->nlink--;
    iupdate(xip);
    iunlockput(xip);
  }

  // Point newname's dirent (freshly emptied above if it existed, or a
  // never-used slot dirlink() finds on its own otherwise) at ip, then
  // zero oldname's - the actual move. Directly overwriting newoff
  // instead of calling dirlink() when xip existed, since dirlink()
  // itself refuses a name that's already present and doesn't know
  // this slot was just freed for reuse.
  if(xip != 0){
    struct dirent de;
    memset(&de, 0, sizeof(de));
    de.inum = ip->inum;
    strncpy(de.name, newname, DIRSIZ);
    if(writei(newdp, (char*)&de, newoff, sizeof(de)) != sizeof(de))
      panic("rename: writei replace");
  } else if(dirlink(newdp, newname, ip->inum) < 0){
    iunlockput(newdp);
    iput(olddp);
    iput(ip);
    end_op();
    return -1;
  }
  int moved_across_dirs = !samefile(olddp, newdp);
  if(ip_is_dir && moved_across_dirs){
    newdp->nlink++;
    iupdate(newdp);
  }
  iunlockput(newdp);

  ilock(olddp);
  struct dirent de;
  memset(&de, 0, sizeof(de));
  if(writei(olddp, (char*)&de, oldoff, sizeof(de)) != sizeof(de))
    panic("rename: writei clear old");
  if(ip_is_dir && moved_across_dirs){
    olddp->nlink--;
    iupdate(olddp);
  }
  iunlockput(olddp);

  if(ip_is_dir && moved_across_dirs){
    // Fix up the moved directory's own ".." to point at its new
    // parent - the one piece of ip's own content a move (unlike a
    // plain rename within the same parent) has to change.
    ilock(ip);
    uint dotdotoff;
    struct inode *olddotdot = dirlookup(ip, "..", &dotdotoff);
    if(olddotdot != 0)
      iput(olddotdot);
    struct dirent dotdot;
    memset(&dotdot, 0, sizeof(dotdot));
    dotdot.inum = newdp->inum;
    strncpy(dotdot.name, "..", DIRSIZ);
    if(writei(ip, (char*)&dotdot, dotdotoff, sizeof(dotdot)) != sizeof(dotdot))
      panic("rename: writei ..");
    iunlock(ip);
  }

  iput(ip);
  end_op();
  return 0;
}

// Shared by sys_open (O_CREATE), sys_mkdir, and sys_mknod: looks up the
// parent directory, creates a new inode of the given type if the name
// doesn't already exist (or, for plain files, returns the existing
// inode if it does - that's what makes O_CREATE idempotent), and links
// it into the parent directory.
static struct inode*
create(char *path, short type, short major, short minor, int mode)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];
  struct proc *curproc = myproc();

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  // Only reached when actually allocating a new inode (the idempotent
  // reopen-existing-file path above never gets here) - need write+search
  // permission on the containing directory, standard Unix semantics
  // ("create a file here" is a permission on the directory, not on a
  // not-yet-existing file).
  if(permcheck(dp, curproc->euid, curproc->egid, PERM_W|PERM_X) < 0){
    iunlockput(dp);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  ip->uid = curproc->euid;
  ip->gid = curproc->egid;
  // Device nodes get a fixed, conventional mode regardless of caller -
  // sys_mknod() has no real mode argument to thread through anyway (see
  // sys_mknod()'s own comment). Everything else honors the caller's
  // requested mode, minus umask, real umask(2)/open(2)/mkdir(2) semantics.
  ip->mode = (type == T_DEV) ? 0666 : (mode & ~(int)curproc->umask & 0777);
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": every directory entry pointing at this
    // inode normally bumps nlink by one, but "." is a self-reference,
    // and counting it would mean an empty directory's nlink is 2 (one
    // real link from its parent, one from its own "..") instead of the
    // 1 that isdirempty()/sys_unlink() expect - and would leave nlink
    // stuck above 0 (leaking the inode) even after the directory's only
    // real link is removed.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

// namei() returning "not found" below is the one sys_open() case that
// needs to be told apart from every other failure (see the #define
// ENOENT comment near the top of this file) - cp/mv's own gnulib
// copy.c checks errno==ENOENT constantly to decide "this destination
// doesn't exist yet, create it" vs. a real error worth reporting.
int
sys_open(void)
{
  char *path;
  int fd, omode, mode;
  int readable, writable;
  struct file *f;
  struct inode *ip;
  struct proc *curproc = myproc();

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    // musl's real open() (musl/src/fcntl/open.c) always issues a 3-arg
    // syscall(SYS_open, path, flags, mode) - mode==0 whenever O_CREAT
    // isn't set, a real mode whenever it is. Default to a permissive
    // 0666 (matches real open(2)'s own documented default when a caller
    // somehow omits the argument) rather than failing outright if it's
    // missing for any reason.
    if(argint(2, &mode) < 0)
      mode = 0666;
    ip = create(path, T_FILE, 0, 0, mode);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -ENOENT;
    }
    ilock(ip);
    // Only the access-mode bits (O_WRONLY/O_RDWR - numerically identical
    // in xv6's own include/fcntl.h and every musl/Linux ABI, since POSIX
    // fixes O_RDONLY/O_WRONLY/O_RDWR at 0/1/2) actually matter here: a
    // directory can never be opened for writing, but any of musl's other
    // flag bits (O_LARGEFILE - unconditionally ORed into every open() by
    // musl/src/internal/syscall.h's __sys_open3, even a plain O_RDONLY
    // one; O_DIRECTORY/O_NOFOLLOW/O_CLOEXEC - opendir()/fts.c's real
    // diropen() sequence) are just flags this kernel doesn't otherwise
    // interpret, not a sign of a write attempt. The old exact
    // `omode != O_RDONLY` check rejected every single directory open
    // from a musl-linked binary (O_LARGEFILE alone made omode nonzero),
    // never noticed before because no earlier coreutils utility
    // (true/false/cat/echo/basename/dirname/yes) ever opened a directory
    // node itself.
    if(ip->type == T_DIR && (omode & (O_WRONLY|O_RDWR))){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  // Applied uniformly to both branches above - a freshly created file is
  // always owned by the caller (trivially passes), and an existing file
  // reopened via O_CREATE|O_EXCL-less idempotence still needs its real
  // permission bits checked, exactly like any other open of an existing
  // file.
  readable = !(omode & O_WRONLY);
  writable = (omode & O_WRONLY) || (omode & O_RDWR);
  if(permcheck(ip, curproc->euid, curproc->egid,
               (readable ? PERM_R : 0) | (writable ? PERM_W : 0)) < 0){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = readable;
  f->writable = writable;
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  int mode;
  struct inode *ip;

  begin_op();
  // musl's real mkdir() (musl/src/stat/mkdir.c) always issues
  // syscall(SYS_mkdir, path, mode) - 2 args.
  if(argstr(0, &path) < 0 || argint(1, &mode) < 0 ||
     (ip = create(path, T_DIR, 0, 0, mode)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int major, minor;

  begin_op();
  // No real mode argument here (deliberately - see kernel/sysfile.c's
  // own comment near create()'s T_DEV case above and include/syscall.h's
  // SYS_mknod comment: poc-os's only real caller uses its own
  // (path, major, minor) convention, not musl's POSIX (path, mode, dev)
  // shape). create() forces a fixed 0666 for T_DEV regardless.
  if((argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();
  
  begin_op();
  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  // namei()'s own directory-traversal check (kernel/fs.c's namex()) only
  // covers ancestors on the way to ip, not ip itself as a destination -
  // chdir needs its own explicit check on the final component.
  if(permcheck(ip, curproc->euid, curproc->egid, PERM_X) < 0){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

// (fd): fchdir()'s real backing syscall - same as sys_chdir() above,
// just taking an already-open directory fd's inode (via idup(), since
// curproc->cwd needs its own reference, separate from the file table
// entry's) instead of doing a fresh namei() path lookup. Needed for
// poc-os's own openat()/fstatat()/etc AT_FDCWD-only emulation
// (coreutils/poc/coreutils_shims.c) to have a genuine fchdir() to sit
// on top of - gnulib's fts.c/canonicalize.c call it directly.
int
sys_fchdir(void)
{
  struct file *f;
  struct inode *ip;
  struct proc *curproc = myproc();

  if(argfd(0, 0, &f) < 0)
    return -1;
  if(f->type != FD_INODE || f->ip->type != T_DIR)
    return -1;

  begin_op();
  ip = f->ip;
  ilock(ip);
  if(permcheck(ip, curproc->euid, curproc->egid, PERM_X) < 0){
    iunlock(ip);
    end_op();
    return -1;
  }
  idup(ip);
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

// (fd, length): see include/syscall.h and kernel/fs.c's itruncto() -
// this is just argument checking, same division of labor as every
// other sys_* wrapper in this file. length is read via argint() (a
// plain int, not a 64-bit off_t) like sys_lseek()'s offset above -
// MAXFILE*BSIZE is a few MB, nowhere near INT_MAX, so poc-os has no
// file a 32-bit length couldn't represent anyway.
int
sys_ftruncate(void)
{
  struct file *f;
  int length;

  if(argfd(0, 0, &f) < 0 || argint(1, &length) < 0)
    return -1;
  if(f->type != FD_INODE || f->ip->type != T_FILE || length < 0)
    return -1;
  if(!f->writable)
    return -1;

  begin_op();
  ilock(f->ip);
  int r = itruncto(f->ip, (uint)length);
  iunlock(f->ip);
  end_op();
  return r;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    // sizeof(uintp), not a hardcoded 4: a 64-bit user binary's own
    // char *argv[] array has 8-byte pointer slots, not 4-byte ones -
    // the same stride mismatch that bit kernel/vectors.pl's output
    // (see trap.c's extern uintp vectors[]).
    if(fetchint(uargv+sizeof(uintp)*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

// Fetches a NUL-terminated, NULL-terminated-array-of-pointers argument
// (argv or envp) the same way sys_exec()'s loop above does, into an
// already zeroed uintp-per-slot array of the given capacity.
static int
fetchargv(uint uarr, char **arr, int cap)
{
  int i;
  uint uarg;

  for(i=0;; i++){
    if(i >= cap)
      return -1;
    if(fetchint(uarr+sizeof(uintp)*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      arr[i] = 0;
      return 0;
    }
    if(fetchstr(uarg, &arr[i]) < 0)
      return -1;
  }
}

int
sys_execve(void)
{
  char *path, *argv[MAXARG], *envp[MAXENVP];
  uint uargv, uenvp;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0 || argint(2, (int*)&uenvp) < 0)
    return -1;
  memset(argv, 0, sizeof(argv));
  memset(envp, 0, sizeof(envp));
  if(fetchargv(uargv, argv, NELEM(argv)) < 0)
    return -1;
  // A NULL envp is a real, valid execve() argument (execve() itself
  // already treats it as "zero entries", not an error) - unlike argv,
  // which every real caller always supplies a real (possibly empty)
  // array for, so no such check exists above. Without this,
  // fetchargv(0, ...) tries to fetch a user-memory word from address
  // 0 and fails, silently turning a valid execve(path, argv, NULL)
  // into an unconditional -1 before execve() itself ever runs.
  if(uenvp && fetchargv(uenvp, envp, NELEM(envp)) < 0)
    return -1;
  return execve(path, argv, envp);
}

// chmod(2)/fchmod(2): owner or root may change permission bits (the low
// 12 bits - rwxrwxrwx plus setuid/setgid/sticky).
int
sys_chmod(void)
{
  char *path;
  int mode;
  struct inode *ip;
  struct proc *curproc = myproc();

  if(argstr(0, &path) < 0 || argint(1, &mode) < 0)
    return -1;

  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(curproc->euid != 0 && curproc->euid != ip->uid){
    iunlockput(ip);
    end_op();
    return -1;
  }
  ip->mode = (ip->mode & ~07777) | (mode & 07777);
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_fchmod(void)
{
  int mode;
  struct file *f;
  struct proc *curproc = myproc();

  if(argfd(0, 0, &f) < 0 || argint(1, &mode) < 0)
    return -1;
  if(f->type != FD_INODE)
    return -1;

  begin_op();
  ilock(f->ip);
  if(curproc->euid != 0 && curproc->euid != f->ip->uid){
    iunlock(f->ip);
    end_op();
    return -1;
  }
  f->ip->mode = (f->ip->mode & ~07777) | (mode & 07777);
  iupdate(f->ip);
  iunlock(f->ip);
  end_op();
  return 0;
}

// chown(2)/fchown(2): root-only (the standard chown_restricted default -
// unprivileged users may not give a file away). uid==-1/gid==-1 means
// "leave that field unchanged", matching real chown()/fchown() semantics
// coreutils' chown/chgrp already rely on.
int
sys_chown(void)
{
  char *path;
  int uid, gid;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &uid) < 0 || argint(2, &gid) < 0)
    return -1;
  if(myproc()->euid != 0)
    return -1;

  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(uid != -1)
    ip->uid = uid;
  if(gid != -1)
    ip->gid = gid;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_fchown(void)
{
  int uid, gid;
  struct file *f;

  if(argfd(0, 0, &f) < 0 || argint(1, &uid) < 0 || argint(2, &gid) < 0)
    return -1;
  if(f->type != FD_INODE)
    return -1;
  if(myproc()->euid != 0)
    return -1;

  begin_op();
  ilock(f->ip);
  if(uid != -1)
    f->ip->uid = uid;
  if(gid != -1)
    f->ip->gid = gid;
  iupdate(f->ip);
  iunlock(f->ip);
  end_op();
  return 0;
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      myproc()->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}
