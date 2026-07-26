/*
 * See ramfs.h. A flat table of nodes (same fixed-array-plus-linear-scan
 * style as process.c's process table) rather than fat32.c's cluster
 * chains -- there's no on-disk layout to respect, so a directory's
 * children are just "every in-use node whose parent field points at it",
 * found by scanning the whole table instead of walking a chain.
 */
#include <stddef.h>
#include "ramfs.h"
#include "heap.h"
#include "string.h"

#define RAMFS_MAX_NODES 512
#define RAMFS_PATH_MAX 128

struct ramfs_node {
    int in_use;
    int is_dir;
    int parent;                       /* index of parent dir, meaningless for node 0 (root) */
    char name[RAMFS_NAME_MAX + 1];
    uint8_t *data;                     /* heap buffer, files only; NULL until first write */
    uint64_t size;                     /* bytes actually holding data */
    uint64_t capacity;                 /* bytes allocated at `data` */
};

static struct ramfs_node nodes[RAMFS_MAX_NODES];

#define RAMFS_ROOT 0

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int str_eq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == b[i];
}

void ramfs_init(void) {
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        nodes[i].in_use = 0;
        nodes[i].data = NULL;
    }
    nodes[RAMFS_ROOT].in_use = 1;
    nodes[RAMFS_ROOT].is_dir = 1;
    nodes[RAMFS_ROOT].parent = RAMFS_ROOT;
    nodes[RAMFS_ROOT].name[0] = '\0';
    nodes[RAMFS_ROOT].size = 0;
    nodes[RAMFS_ROOT].capacity = 0;
}

static int alloc_node(void) {
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].in_use) {
            return i;
        }
    }
    return -1;
}

static int find_child(int parent, const char *name) {
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].in_use && nodes[i].parent == parent && i != RAMFS_ROOT && str_eq(nodes[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/* Same splitting rules as fat32.c's own split_parent(): the parent is
 * everything before the last '/', collapsing to "/" if that's empty
 * (path was "/name"); name_out points into `path` itself. */
static int split_parent(const char *path, char *parent_out, uint64_t parent_cap, const char **name_out) {
    const char *last_slash = NULL;
    for (const char *q = path; *q; q++) {
        if (*q == '/') {
            last_slash = q;
        }
    }
    if (last_slash == NULL) {
        return 0;
    }
    uint64_t parent_len = (uint64_t)(last_slash - path);
    if (parent_len == 0) {
        if (parent_cap < 2) {
            return 0;
        }
        parent_out[0] = '/';
        parent_out[1] = '\0';
    } else {
        if (parent_len + 1 > parent_cap) {
            return 0;
        }
        memcpy(parent_out, path, parent_len);
        parent_out[parent_len] = '\0';
    }
    *name_out = last_slash + 1;
    return **name_out != '\0';
}

static void fill_out(struct fat32_file *out, int node) {
    out->first_cluster = (uint32_t)node;
    out->size = (uint32_t)nodes[node].size;
    out->is_dir = nodes[node].is_dir;
    out->dirent_cluster = 0;
    out->dirent_offset = 0;
}

int ramfs_lookup(const char *path, struct fat32_file *out) {
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        fill_out(out, RAMFS_ROOT);
        return 1;
    }

    int parent = RAMFS_ROOT;
    int found = -1;
    while (*p) {
        uint64_t len = 0;
        while (p[len] && p[len] != '/') {
            len++;
        }
        if (len == 0 || len > RAMFS_NAME_MAX) {
            return 0;
        }
        char comp[RAMFS_NAME_MAX + 1];
        memcpy(comp, p, len);
        comp[len] = '\0';
        p += len;
        while (*p == '/') {
            p++;
        }

        found = find_child(parent, comp);
        if (found < 0) {
            return 0;
        }
        if (*p != '\0') {
            if (!nodes[found].is_dir) {
                return 0; /* tried to descend into a file */
            }
            parent = found;
        }
    }

    fill_out(out, found);
    return 1;
}

int ramfs_create(const char *path, struct fat32_file *out) {
    char parent_path[RAMFS_PATH_MAX];
    const char *name;
    if (!split_parent(path, parent_path, sizeof(parent_path), &name) || str_len(name) > RAMFS_NAME_MAX) {
        return 0;
    }

    struct fat32_file parent;
    if (!ramfs_lookup(parent_path, &parent) || !parent.is_dir) {
        return 0;
    }
    int parent_node = (int)parent.first_cluster;

    int existing = find_child(parent_node, name);
    if (existing >= 0) {
        if (nodes[existing].is_dir) {
            return 0; /* can't create a regular file over an existing directory */
        }
        if (nodes[existing].data != NULL) {
            kfree(nodes[existing].data);
            nodes[existing].data = NULL;
        }
        nodes[existing].size = 0;
        nodes[existing].capacity = 0;
        fill_out(out, existing);
        return 1;
    }

    int slot = alloc_node();
    if (slot < 0) {
        return 0;
    }
    nodes[slot].in_use = 1;
    nodes[slot].is_dir = 0;
    nodes[slot].parent = parent_node;
    memcpy(nodes[slot].name, name, str_len(name) + 1);
    nodes[slot].data = NULL;
    nodes[slot].size = 0;
    nodes[slot].capacity = 0;

    fill_out(out, slot);
    return 1;
}

int ramfs_mkdir(const char *path) {
    char parent_path[RAMFS_PATH_MAX];
    const char *name;
    if (!split_parent(path, parent_path, sizeof(parent_path), &name) || str_len(name) > RAMFS_NAME_MAX) {
        return 0;
    }

    struct fat32_file parent;
    if (!ramfs_lookup(parent_path, &parent) || !parent.is_dir) {
        return 0;
    }
    int parent_node = (int)parent.first_cluster;

    if (find_child(parent_node, name) >= 0) {
        return 0; /* already exists */
    }

    int slot = alloc_node();
    if (slot < 0) {
        return 0;
    }
    nodes[slot].in_use = 1;
    nodes[slot].is_dir = 1;
    nodes[slot].parent = parent_node;
    memcpy(nodes[slot].name, name, str_len(name) + 1);
    nodes[slot].data = NULL;
    nodes[slot].size = 0;
    nodes[slot].capacity = 0;
    return 1;
}

int ramfs_readdir(struct fat32_file *dir, uint32_t index, char *name_out, uint32_t *size_out, int *is_dir_out) {
    int dir_node = (int)dir->first_cluster;
    uint32_t seen = 0;
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].in_use || i == RAMFS_ROOT || nodes[i].parent != dir_node) {
            continue;
        }
        if (seen == index) {
            uint64_t len = str_len(nodes[i].name);
            memcpy(name_out, nodes[i].name, len + 1);
            *size_out = (uint32_t)nodes[i].size;
            *is_dir_out = nodes[i].is_dir;
            return FAT32_DIRENT_VALID;
        }
        seen++;
    }
    return FAT32_DIRENT_END;
}

int ramfs_unlink(const char *path) {
    struct fat32_file f;
    if (!ramfs_lookup(path, &f) || f.is_dir) {
        return 0;
    }
    int node = (int)f.first_cluster;
    if (nodes[node].data != NULL) {
        kfree(nodes[node].data);
    }
    nodes[node].in_use = 0;
    return 1;
}

int ramfs_rmdir(const char *path) {
    struct fat32_file f;
    if (!ramfs_lookup(path, &f) || !f.is_dir) {
        return 0;
    }
    int node = (int)f.first_cluster;
    if (node == RAMFS_ROOT) {
        return 0;
    }
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].in_use && nodes[i].parent == node) {
            return 0; /* not empty */
        }
    }
    nodes[node].in_use = 0;
    return 1;
}

int ramfs_rename(const char *old_path, const char *new_path) {
    struct fat32_file old_f;
    if (!ramfs_lookup(old_path, &old_f) || (int)old_f.first_cluster == RAMFS_ROOT) {
        return 0; /* doesn't exist, or is the root directory */
    }
    int old_node = (int)old_f.first_cluster;

    char parent_path[RAMFS_PATH_MAX];
    const char *name;
    if (!split_parent(new_path, parent_path, sizeof(parent_path), &name) || str_len(name) > RAMFS_NAME_MAX) {
        return 0;
    }
    struct fat32_file parent;
    if (!ramfs_lookup(parent_path, &parent) || !parent.is_dir) {
        return 0;
    }
    int parent_node = (int)parent.first_cluster;

    int existing = find_child(parent_node, name);
    if (existing >= 0) {
        if (existing == old_node) {
            return 1; /* renaming a path onto itself -- nothing to do */
        }
        if (nodes[existing].is_dir) {
            return 0; /* never overwrite a directory */
        }
        if (nodes[existing].data != NULL) {
            kfree(nodes[existing].data);
        }
        nodes[existing].in_use = 0;
    }

    nodes[old_node].parent = parent_node;
    memcpy(nodes[old_node].name, name, str_len(name) + 1);
    return 1;
}

int64_t ramfs_read(struct fat32_file *f, uint64_t offset, void *buf, uint64_t len) {
    int node = (int)f->first_cluster;
    if (offset >= nodes[node].size) {
        return 0;
    }
    uint64_t remaining = nodes[node].size - offset;
    uint64_t n = (len < remaining) ? len : remaining;
    memcpy(buf, nodes[node].data + offset, n);
    return (int64_t)n;
}

int64_t ramfs_write(struct fat32_file *f, uint64_t offset, const void *buf, uint64_t len) {
    int node = (int)f->first_cluster;
    if (len == 0) {
        return 0;
    }
    uint64_t needed = offset + len;

    if (needed > nodes[node].capacity) {
        uint64_t new_cap = (nodes[node].capacity != 0) ? nodes[node].capacity : 4096;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        uint8_t *new_data = (uint8_t *)kmalloc(new_cap);
        if (new_data == NULL) {
            return -1;
        }
        if (nodes[node].data != NULL) {
            memcpy(new_data, nodes[node].data, nodes[node].size);
            kfree(nodes[node].data);
        }
        nodes[node].data = new_data;
        nodes[node].capacity = new_cap;
    }
    /* Zero-fill a sparse write's gap (offset past the current size),
     * matching fat32_write()'s own contract -- must happen whether or
     * not the branch above just grew the buffer, since an in-capacity
     * write can still start past the current size. */
    if (offset > nodes[node].size) {
        memset(nodes[node].data + nodes[node].size, 0, offset - nodes[node].size);
    }

    memcpy(nodes[node].data + offset, buf, len);
    if (needed > nodes[node].size) {
        nodes[node].size = needed;
    }
    f->size = (uint32_t)nodes[node].size;
    return (int64_t)len;
}
