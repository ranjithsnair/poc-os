/* poc-os stand-in for the real <selinux/label.h> (libselinux) - see
 * selinux.h in this same directory for why this exists at all.
 * struct selabel_handle stays opaque (coreutils/src/selinux.h, already
 * vendored, only ever stores a pointer to one, never dereferences a
 * member), matching real libselinux's own public API shape.
 */
#ifndef POC_SELINUX_LABEL_H
#define POC_SELINUX_LABEL_H

/* Real libselinux's <selinux/label.h> pulls in is_selinux_enabled()/
 * setfscreatecon() transitively via <selinux/selinux.h> - mkdir.c/
 * mv.c/cp.c call those without including selinux.h themselves,
 * relying on exactly that. */
#include <selinux/selinux.h>

#define SELABEL_CTX_FILE 0

struct selabel_handle;

struct selabel_handle *selabel_open(unsigned int backend, void *options, unsigned nopts);
void selabel_close(struct selabel_handle *handle);
int selabel_lookup_raw(struct selabel_handle *handle, char **con, const char *key, int type);

#endif
