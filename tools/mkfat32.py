#!/usr/bin/env python3
"""
Builds a FAT32 disk image from a source directory tree, matching the
on-disk layout kernel/src/fat32.c reads/writes: 512-byte sectors, 4KiB
(8-sector) clusters, a single FAT (no redundant second copy -- this is a
private disk for our own kernel, not something meant to survive a real
fsck), and short (8.3) names only -- no VFAT long-filename entries.

Every BPB field fat32.c depends on is written out explicitly here (rather
than assumed) precisely so the two stay in sync by construction: change a
constant below and the kernel driver picks it up from the boot sector at
mount time instead of needing a matching hardcoded change.

Usage: mkfat32.py <output.img> <size_mib> <source_dir>
"""
import os
import struct
import sys

BYTES_PER_SECTOR = 512
SECTORS_PER_CLUSTER = 8
RESERVED_SECTORS = 32
NUM_FATS = 1
FSINFO_SECTOR = 1
ROOT_CLUSTER = 2
MEDIA_DESCRIPTOR = 0xF8

FAT_FREE = 0x00000000
FAT_EOC = 0x0FFFFFFF
FAT_MASK = 0x0FFFFFFF


def fat_size_sectors(total_sectors):
    # Standard FAT32 FAT-size formula (Microsoft fatgen103), specialized
    # to RootDirSectors = 0 (always true for FAT32) and our NUM_FATS.
    tmp1 = total_sectors - RESERVED_SECTORS
    tmp2 = (256 * SECTORS_PER_CLUSTER) + NUM_FATS
    tmp2 = tmp2 // 2
    return (tmp1 + (tmp2 - 1)) // tmp2


def short_name(name):
    """Splits 'name.ext' into an 11-byte 8.3 short-name field. Rejects
    anything that doesn't fit -- long-filename entries aren't
    implemented, so every name in the source tree must already be
    8.3-compatible."""
    if name in (".", ".."):
        base, ext = name, ""
    elif "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    if len(base) > 8 or len(ext) > 3:
        raise ValueError(f"{name!r} is not 8.3-compatible")
    base = base.upper().ljust(8)
    ext = ext.upper().ljust(3)
    return (base + ext).encode("ascii")


class Fat32Image:
    def __init__(self, total_sectors):
        self.total_sectors = total_sectors
        self.fat_size = fat_size_sectors(total_sectors)
        self.data_start_lba = RESERVED_SECTORS + NUM_FATS * self.fat_size
        self.total_clusters = (total_sectors - self.data_start_lba) // SECTORS_PER_CLUSTER
        if self.total_clusters < 65525:
            raise ValueError("image too small to be valid FAT32 (need >= 65525 clusters)")

        self.fat = bytearray(self.fat_size * BYTES_PER_SECTOR)
        self._set_fat(0, 0x0FFFFFF8)
        self._set_fat(1, FAT_EOC)
        # ROOT_CLUSTER (2) is the root directory's own cluster, allocated
        # implicitly rather than through alloc_cluster() -- reserve it in
        # the FAT and start handing out fresh clusters after it, or the
        # first file/subdirectory created would collide with the root
        # directory's own storage.
        self._set_fat(ROOT_CLUSTER, FAT_EOC)
        self.next_free_cluster = ROOT_CLUSTER + 1

        self.data = bytearray(self.total_clusters * SECTORS_PER_CLUSTER * BYTES_PER_SECTOR)

    def _set_fat(self, cluster, value):
        off = cluster * 4
        struct.pack_into("<I", self.fat, off, value & 0xFFFFFFFF)

    def alloc_cluster(self):
        c = self.next_free_cluster
        self.next_free_cluster += 1
        if self.next_free_cluster > self.total_clusters + 1:
            raise ValueError("out of clusters -- increase the image size")
        self._set_fat(c, FAT_EOC)
        return c

    def cluster_bytes(self, cluster):
        off = (cluster - 2) * SECTORS_PER_CLUSTER * BYTES_PER_SECTOR
        return memoryview(self.data)[off: off + SECTORS_PER_CLUSTER * BYTES_PER_SECTOR]

    def write_file_data(self, content):
        """Writes `content` into a freshly allocated cluster chain,
        returns (first_cluster, size). first_cluster is 0 for an empty
        file (FAT32 leaves zero-length files with no allocated cluster
        at all)."""
        if len(content) == 0:
            return 0, 0
        cluster_size = SECTORS_PER_CLUSTER * BYTES_PER_SECTOR
        first = None
        prev = None
        off = 0
        while off < len(content):
            c = self.alloc_cluster()
            if first is None:
                first = c
            if prev is not None:
                self._set_fat(prev, c)
            chunk = content[off: off + cluster_size]
            self.cluster_bytes(c)[: len(chunk)] = chunk
            prev = c
            off += cluster_size
        return first, len(content)

    def write_dirents(self, cluster, entries):
        """Writes a list of 32-byte directory entries into `cluster`'s
        chain, allocating additional clusters if they don't all fit.
        Assumes the directory is being created fresh (no pre-existing
        entries to preserve)."""
        cluster_size = SECTORS_PER_CLUSTER * BYTES_PER_SECTOR
        entries_per_cluster = cluster_size // 32
        blob = b"".join(entries)
        # Zero-pad so unused slots are all-zero (0x00 first byte = "end
        # of directory", which fat32.c treats as "no more entries").
        needed_clusters = max(1, (len(blob) + cluster_size - 1) // cluster_size)
        blob = blob.ljust(needed_clusters * cluster_size, b"\x00")

        cur = cluster
        for i in range(needed_clusters):
            self.cluster_bytes(cur)[:] = blob[i * cluster_size: (i + 1) * cluster_size]
            if i < needed_clusters - 1:
                nxt = self.alloc_cluster()
                self._set_fat(cur, nxt)
                cur = nxt
        _ = entries_per_cluster  # not otherwise needed, kept for clarity

    def make_dirent(self, name, attr, first_cluster, size):
        raw = bytearray(32)
        raw[0:11] = short_name(name)
        raw[0x0B] = attr
        struct.pack_into("<H", raw, 0x14, (first_cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", raw, 0x1A, first_cluster & 0xFFFF)
        struct.pack_into("<I", raw, 0x1C, size)
        return bytes(raw)

    def add_tree(self, dir_cluster, src_dir):
        entries = []
        for name in sorted(os.listdir(src_dir)):
            path = os.path.join(src_dir, name)
            if os.path.isdir(path):
                child_cluster = self.alloc_cluster()
                entries.append(self.make_dirent(name, 0x10, child_cluster, 0))
                self.add_tree(child_cluster, path)
            else:
                with open(path, "rb") as f:
                    content = f.read()
                first_cluster, size = self.write_file_data(content)
                entries.append(self.make_dirent(name, 0x20, first_cluster, size))
        self.write_dirents(dir_cluster, entries)

    def boot_sector(self):
        b = bytearray(BYTES_PER_SECTOR)
        b[0:3] = b"\xEB\x3C\x90"
        b[3:11] = b"POCOS4.0"
        struct.pack_into("<H", b, 0x0B, BYTES_PER_SECTOR)
        b[0x0D] = SECTORS_PER_CLUSTER
        struct.pack_into("<H", b, 0x0E, RESERVED_SECTORS)
        b[0x10] = NUM_FATS
        struct.pack_into("<H", b, 0x11, 0)          # root_entry_count -- 0 for FAT32
        struct.pack_into("<H", b, 0x13, 0)          # total_sectors_16 -- 0, using the 32-bit field
        b[0x15] = MEDIA_DESCRIPTOR
        struct.pack_into("<H", b, 0x16, 0)          # fat_size_16 -- 0, using fat_size_32
        struct.pack_into("<H", b, 0x18, 32)         # sectors_per_track (unused by fat32.c)
        struct.pack_into("<H", b, 0x1A, 64)         # num_heads (unused by fat32.c)
        struct.pack_into("<I", b, 0x1C, 0)          # hidden_sectors
        struct.pack_into("<I", b, 0x20, self.total_sectors)
        struct.pack_into("<I", b, 0x24, self.fat_size)
        struct.pack_into("<H", b, 0x28, 0)          # ext_flags
        struct.pack_into("<H", b, 0x2A, 0)          # fs_version
        struct.pack_into("<I", b, 0x2C, ROOT_CLUSTER)
        struct.pack_into("<H", b, 0x30, FSINFO_SECTOR)
        struct.pack_into("<H", b, 0x32, 0)          # backup_boot_sector -- none
        b[0x40] = 0x80                              # drive_number
        b[0x42] = 0x29                              # boot_signature
        struct.pack_into("<I", b, 0x43, 0x504F4321)  # volume_id
        b[0x47:0x52] = b"POC-OS DISK"
        b[0x52:0x5A] = b"FAT32   "
        b[0x1FE:0x200] = b"\x55\xAA"
        return bytes(b)

    def fsinfo_sector(self):
        b = bytearray(BYTES_PER_SECTOR)
        struct.pack_into("<I", b, 0x00, 0x41615252)  # lead signature
        struct.pack_into("<I", b, 0x1E4, 0x61417272)  # struct signature
        struct.pack_into("<I", b, 0x1E8, 0xFFFFFFFF)  # free cluster count -- unknown, fat32.c doesn't trust this
        struct.pack_into("<I", b, 0x1EC, 0xFFFFFFFF)  # next free cluster hint -- unknown
        struct.pack_into("<I", b, 0x1FC, 0xAA550000)  # trail signature
        return bytes(b)

    def build(self, out_path, src_dir):
        self.add_tree(ROOT_CLUSTER, src_dir)

        with open(out_path, "wb") as f:
            f.write(self.boot_sector())
            f.write(self.fsinfo_sector())
            f.write(b"\x00" * BYTES_PER_SECTOR * (RESERVED_SECTORS - 2))
            f.write(self.fat)
            f.write(self.data)
            total_bytes = self.total_sectors * BYTES_PER_SECTOR
            written = f.tell()
            if written < total_bytes:
                f.write(b"\x00" * (total_bytes - written))


def main():
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <output.img> <size_mib> <source_dir>", file=sys.stderr)
        sys.exit(1)
    out_path, size_mib, src_dir = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    total_sectors = (size_mib * 1024 * 1024) // BYTES_PER_SECTOR
    img = Fat32Image(total_sectors)
    img.build(out_path, src_dir)
    print(f"{out_path}: {size_mib} MiB, {img.total_clusters} clusters, "
          f"FAT size {img.fat_size} sectors, data starts at LBA {img.data_start_lba}")


if __name__ == "__main__":
    main()
