/*
 * Not part of mlibc itself (its <paths.h> only ships under the glibc
 * option, which we don't enable -- see meson.build's doc comment, and
 * sys/ioctl.h's for the same reasoning applied to bash). busybox's
 * libbb.h includes <paths.h> unconditionally, the way it would on any
 * real Unix, for a handful of _PATH_* string constants -- copied
 * verbatim from mlibc's own options/glibc/include/paths.h (itself taken
 * from musl) rather than reinvented, since none of it is actually
 * glibc-specific. A few of these name things this kernel doesn't have
 * (/proc, /var, a passwd database) -- left as-is anyway: they're just
 * string literals nothing resolves unless an applet that needs that
 * particular path is actually built (see toolchain/busybox-pocos.config).
 */
#ifndef _PATHS_H
#define _PATHS_H

#define	_PATH_DEFPATH	"/usr/local/bin:/bin:/usr/bin"
#define	_PATH_STDPATH	"/bin:/usr/bin:/sbin:/usr/sbin"

#define	_PATH_BSHELL	"/bin/sh"
#define	_PATH_CONSOLE	"/dev/console"
#define	_PATH_DEVNULL	"/dev/null"
#define _PATH_GSHADOW	"/etc/gshadow"
#define	_PATH_KLOG	"/proc/kmsg"
#define	_PATH_LASTLOG	"/var/log/lastlog"
#define	_PATH_MAILDIR	"/var/mail"
#define	_PATH_MAN	"/usr/share/man"
#define	_PATH_MNTTAB	"/etc/fstab"
#define	_PATH_MOUNTED	"/etc/mtab"
#define	_PATH_NOLOGIN	"/etc/nologin"
#define _PATH_PRESERVE	"/var/lib"
#define	_PATH_SENDMAIL	"/usr/sbin/sendmail"
#define	_PATH_SHADOW	"/etc/shadow"
#define	_PATH_SHELLS	"/etc/shells"
#define	_PATH_TTY	"/dev/tty"
#define _PATH_UTMP	"/var/run/utmp"
#define	_PATH_VI	"/usr/bin/vi"
#define _PATH_WTMP	"/var/log/wtmp"

#define	_PATH_DEV	"/dev/"
#define	_PATH_TMP	"/tmp/"
#define	_PATH_VARDB	"/var/lib/misc/"
#define	_PATH_VARRUN	"/var/run/"
#define	_PATH_VARTMP	"/var/tmp/"

#ifdef _GNU_SOURCE
#define _PATH_UTMPX _PATH_UTMP
#define _PATH_WTMPX _PATH_WTMP
#endif

#endif /* _PATHS_H */
