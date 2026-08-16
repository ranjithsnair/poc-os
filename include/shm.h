// POSIX-ish shared memory (GUI roadmap phase 3, Wayland's wl_shm
// mechanism: a client mmaps a region, fills it with pixels, and passes
// the *fd* to the compositor over an AF_UNIX socket via SCM_RIGHTS -
// the compositor mmaps the same fd and sees the same bytes).
//
// SYS_shm_create (kernel/shm.c) is a new poc-os-specific syscall, not
// real shm_open(). Real shm_open() is a plain open("/dev/shm/name",
// ...) in musl (musl/src/mman/shm_open.c) with no dedicated syscall at
// all - hooking that path would mean building a tmpfs, out of scope
// for now. This is the place to add a shm_open() shim (matching this
// codebase's established *_shims.c convention) once a real Wayland
// client library is actually being ported; until then, callers use
// syscall(SYS_shm_create, size) directly, the same way bash/poc/
// fbtest.c already calls syscall(SYS_mknod, ...) raw.

#define SHM_MAXOBJS  16
#define SHM_MAXPAGES 64      // 256KB cap per object - plenty for a
                              // cursor or small test surface

// Exactly one struct file ever wraps a given shmobj (created once in
// shmcreate() below; fork()/dup()/SCM_RIGHTS-passing all share that
// same struct file* and its existing f->ref count - see kernel/
// sysnet.c's sys_recvmsg(), which fdalloc()s the very same pointer
// sockrecv() handed it, rather than creating a new struct file). So
// struct file's own ref count is already exactly the refcount this
// object needs; no separate one is kept here.
struct shmobj {
  int inuse;
  uint size;                   // requested size, in bytes
  char *pages[SHM_MAXPAGES];   // kalloc()'d, one page each
};
