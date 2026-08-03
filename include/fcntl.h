#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
// 0100 (not xv6's traditional 0x200): matches musl/Linux's real O_CREAT
// ABI value (musl/arch/generic/bits/fcntl.h) exactly, so a single
// kernel-side omode&O_CREATE check (kernel/sysfile.c's sys_open())
// works for both this header's own native callers (user/sh.c's I/O
// redirection) and every dynamically-linked musl/coreutils binary,
// which sends flag bits from ITS OWN <fcntl.h>, not this one. Before
// this fix the two headers' O_CREATE/O_CREAT bits never overlapped, so
// a musl-linked open(path, O_CREAT|O_WRONLY, mode) silently behaved as
// a plain (non-creating) open - never noticed earlier because no
// coreutils utility shipped so far ever created a new file via open()
// (mkdir/ln/etc all use their own dedicated syscalls); cp/mv are the
// first that do.
#define O_CREATE  0100
