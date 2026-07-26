#!/bin/sh
# Patches a freshly extracted busybox checkout's include/platform.h --
# called by the top-level Makefile's busybox/Makefile rule right after
# extraction, so this is reproducible from a clean checkout (not a
# one-off manual edit).
#
# platform.h assumes a hosted glibc/musl-like Linux environment by
# default (see its own "Assume all these functions and header files
# exist by default" block): mntent.h/sys/statfs.h don't exist in our
# mlibc port at all (no mount/df/findmnt applets are enabled -- see
# toolchain/busybox-pocos.config), and strverscmp()/mempcpy() are glibc
# extensions our port doesn't implement (verified empirically: busybox
# failed to compile/link on each of these before this patch existed).
# platform.h already carries equivalent #undef blocks for BSD/Apple/
# FreeBSD right after that "assume defaults" block; this just adds our
# own there too, unconditionally (this fetched busybox checkout is only
# ever built for this one target) rather than gated on a platform macro
# nothing here defines.
#
# Usage: patch-busybox-platform.sh <path-to-platform.h>
set -eu

platform_h=$1

sed -i '' '/^#define DEV_FD_PREFIX "\/dev\/fd\/"$/a\
\
/* PoC-OS: no mtab\/mount, no Linux-style statfs, no strverscmp()\/mempcpy()\
 * in mlibc -- see toolchain\/busybox-pocos.config and\
 * tools\/patch-busybox-platform.sh'"'"'s own doc comment. */\
#undef HAVE_MNTENT_H\
#undef HAVE_SYS_STATFS_H\
#undef HAVE_STRVERSCMP\
#undef HAVE_MEMPCPY
' "$platform_h"
