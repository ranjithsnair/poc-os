/* poc-os stand-in for the real <selinux/selinux.h> (libselinux) -
 * poc-os has no SELinux, no security_context xattrs, nothing.
 * coreutils/src/{mkdir,mv,cp,stat}.c #include this unconditionally
 * (no HAVE_SELINUX_SELINUX_H guard - see their own top-of-file
 * #includes), the same way a real system either has libselinux-devel
 * installed or doesn't; since we have neither the real headers nor
 * gnulib's own replacement for them, this is that missing piece.
 * is_selinux_enabled() always returning 0 is what makes every one of
 * these callers' "if (is_selinux_enabled() > 0)" branch correctly
 * skip the rest, so getfilecon()/lgetfilecon()/setfscreatecon() are
 * link-time-only requirements here, never actually called - failing
 * loudly (rather than silently faking a context) if one ever is.
 */
#ifndef POC_SELINUX_SELINUX_H
#define POC_SELINUX_SELINUX_H

int is_selinux_enabled(void);
int getfilecon(const char *path, char **con);
int getfilecon_raw(const char *path, char **con);
int lgetfilecon(const char *path, char **con);
int lgetfilecon_raw(const char *path, char **con);
int lsetfilecon_raw(const char *path, const char *con);
int setfscreatecon(const char *context);
int setfscreatecon_raw(const char *context);
void freecon(char *con);

#endif
