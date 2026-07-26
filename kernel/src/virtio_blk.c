/*
 * See virtio_blk.h. Implements just enough of legacy virtio-pci
 * (virtio spec 1.x, section 4.1.4/"Legacy Interfaces") to drive a single
 * virtio-blk device: PCI config space access via the 0xCF8/0xCFC I/O
 * ports (mechanism #1), the legacy I/O-BAR register layout, and one
 * virtqueue used purely synchronously (submit, then busy-poll the used
 * ring -- no interrupt handler, since nothing else here needs concurrent
 * disk I/O).
 */
#include <stdint.h>
#include <stddef.h>
#include "virtio_blk.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "framebuffer.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define VIRTIO_PCI_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID 0x1001 /* legacy/transitional virtio-blk */

/* Legacy virtio-pci register layout, all relative to the I/O-space BAR0
 * base -- see virtio spec 4.1.4.8. Device-specific config (capacity,
 * for virtio-blk) starts right after the fixed common fields. */
#define VIRTIO_REG_DEVICE_FEATURES 0x00
#define VIRTIO_REG_GUEST_FEATURES  0x04
#define VIRTIO_REG_QUEUE_PFN       0x08
#define VIRTIO_REG_QUEUE_SIZE      0x0C
#define VIRTIO_REG_QUEUE_SELECT    0x0E
#define VIRTIO_REG_QUEUE_NOTIFY    0x10
#define VIRTIO_REG_DEVICE_STATUS   0x12
#define VIRTIO_REG_CONFIG          0x14

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04

#define VIRTQ_DESC_F_NEXT  1u
#define VIRTQ_DESC_F_WRITE 2u

/* The "virtqueue" is how the driver and the virtio device pass buffers
 * back and forth: three parallel arrays sharing one block of memory.
 * - virtq_desc: one entry per buffer the driver wants the device to
 *   read from or write to (addr/len = where and how much, `next` chains
 *   several descriptors into one request, e.g. header+data+status below).
 * - virtq_avail: the driver's "here are requests to process" queue --
 *   `ring` holds descriptor-chain head indices, `idx` counts how many
 *   have ever been pushed.
 * - virtq_used: the device's "here's what I finished" queue, same idea
 *   in the other direction. */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
};

#define VIRTIO_BLK_T_IN  0 /* read */
#define VIRTIO_BLK_T_OUT 1 /* write */

struct virtio_blk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static int device_present = 0;
static uint16_t io_base;
static uint16_t queue_size;

static struct virtq_desc *desc;
static struct virtq_avail *avail;
static struct virtq_used *used;
static uint16_t last_used_idx;

/* One dedicated physical frame used as scratch for every request: the
 * request header at offset 0, the 1-byte status at offset 32, and a
 * 512-byte bounce buffer for the data itself at offset 64 -- all comfortably
 * inside one 4KiB frame, and all at a single known physical address
 * regardless of what virtual address the caller's own buffer has (which
 * might not be physically contiguous, unlike this dedicated frame). */
static uint8_t *scratch_virt;
static uint64_t scratch_phys;
#define SCRATCH_HEADER_OFF 0
#define SCRATCH_STATUS_OFF 32
#define SCRATCH_DATA_OFF   64

static uint64_t capacity_sectors;

static uint32_t pci_address(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
           ((uint32_t)fn << 8) | (offset & 0xFCu);
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, dev, fn, offset));
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset) {
    uint32_t dword = pci_read32(bus, dev, fn, offset);
    return (uint16_t)(dword >> ((offset & 2) * 8));
}

/* Writes just the addressed 16-bit half of a config dword, via the
 * well-known CONFIG_DATA sub-offset trick (0xCFC+n reads/writes byte n
 * of whatever dword CONFIG_ADDRESS currently names) -- critically, this
 * leaves the *other* half untouched. That matters here specifically
 * because the command register (0x04) shares its dword with the status
 * register (0x06), whose bits are write-1-to-clear: a read-modify-write
 * of the full dword would silently clear whatever status bits happened
 * to be set. */
static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint16_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, dev, fn, offset));
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), value);
}

static uint64_t align_up(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

/* Finds the first device on bus 0 matching VIRTIO_PCI_VENDOR_ID /
 * VIRTIO_BLK_DEVICE_ID. Returns 1 and fills out_dev/out_fn on success. */
static int find_virtio_blk(uint8_t *out_dev, uint8_t *out_fn) {
    for (uint16_t dev = 0; dev < 32; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint16_t vendor = pci_read16(0, (uint8_t)dev, fn, 0x00);
            if (vendor == 0xFFFF) {
                continue; /* nothing at this dev/fn */
            }
            uint16_t device = pci_read16(0, (uint8_t)dev, fn, 0x02);
            if (vendor == VIRTIO_PCI_VENDOR_ID && device == VIRTIO_BLK_DEVICE_ID) {
                *out_dev = (uint8_t)dev;
                *out_fn = fn;
                return 1;
            }
        }
    }
    return 0;
}

int virtio_blk_init(void) {
    uint8_t dev, fn;
    if (!find_virtio_blk(&dev, &fn)) {
        fb_print("virtio-blk: no device found on PCI bus 0.\n");
        return 0;
    }

    /* Enable I/O space decoding (bit 0) and bus mastering (bit 2) in the
     * PCI command register so the device can both be talked to over its
     * BAR0 I/O ports and DMA into the buffers we hand it. */
    uint16_t command = pci_read16(0, dev, fn, 0x04);
    pci_write16(0, dev, fn, 0x04, command | 0x1 | 0x4);

    uint32_t bar0 = pci_read32(0, dev, fn, 0x10);
    if (!(bar0 & 1)) {
        fb_print("virtio-blk: BAR0 is not an I/O-space BAR, giving up.\n");
        return 0;
    }
    io_base = (uint16_t)(bar0 & ~0x3u);

    /* Standard virtio reset/negotiate dance (spec 3.1.1). Legacy virtio
     * doesn't require FEATURES_OK, and accepting zero optional features
     * still gets the base read/write protocol every virtio-blk device
     * supports. */
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, 0);
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    outl(io_base + VIRTIO_REG_GUEST_FEATURES, 0);

    outw(io_base + VIRTIO_REG_QUEUE_SELECT, 0);
    queue_size = inw(io_base + VIRTIO_REG_QUEUE_SIZE);
    if (queue_size == 0) {
        fb_print("virtio-blk: device reports queue size 0, giving up.\n");
        return 0;
    }

    /* Legacy virtqueue layout (spec 2.6.2): descriptor table + avail ring
     * packed together, then the used ring starting at the next page
     * boundary -- the used ring's alignment is the one hard requirement
     * legacy relies on QueuePFN's page-granularity to express. */
    uint64_t avail_off = 16ull * queue_size;
    uint64_t used_off = align_up(avail_off + 6 + 2ull * queue_size, PMM_FRAME_SIZE);
    uint64_t total_size = used_off + 6 + 8ull * queue_size;
    uint64_t total_pages = (total_size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    /* The device only understands a single contiguous physical run for
     * the whole queue (QueuePFN is one frame number) -- pmm_alloc_frame()
     * only ever hands out one frame at a time, so ask for `total_pages`
     * of them in a row and verify they actually landed contiguously,
     * which they always will called this early at boot (right after
     * pmm_init(), nothing has freed anything yet to fragment the bitmap). */
    uint64_t queue_phys = pmm_alloc_frame();
    if (queue_phys == 0) {
        fb_print("virtio-blk: out of memory allocating the virtqueue.\n");
        return 0;
    }
    for (uint64_t i = 1; i < total_pages; i++) {
        uint64_t next = pmm_alloc_frame();
        if (next != queue_phys + i * PMM_FRAME_SIZE) {
            fb_print("virtio-blk: virtqueue frames weren't contiguous, giving up.\n");
            return 0;
        }
    }

    uint8_t *queue_virt = (uint8_t *)vmm_phys_to_virt(queue_phys);
    desc = (struct virtq_desc *)queue_virt;
    avail = (struct virtq_avail *)(queue_virt + avail_off);
    used = (struct virtq_used *)(queue_virt + used_off);
    last_used_idx = used->idx;

    outl(io_base + VIRTIO_REG_QUEUE_PFN, (uint32_t)(queue_phys / PMM_FRAME_SIZE));
    outb(io_base + VIRTIO_REG_DEVICE_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    scratch_phys = pmm_alloc_frame();
    if (scratch_phys == 0) {
        fb_print("virtio-blk: out of memory allocating the request scratch frame.\n");
        return 0;
    }
    scratch_virt = (uint8_t *)vmm_phys_to_virt(scratch_phys);

    uint32_t cap_lo = inl(io_base + VIRTIO_REG_CONFIG);
    uint32_t cap_hi = inl(io_base + VIRTIO_REG_CONFIG + 4);
    capacity_sectors = ((uint64_t)cap_hi << 32) | cap_lo;

    fb_print("virtio-blk: initialized, capacity ");
    fb_print_dec(capacity_sectors * 512 / (1024 * 1024));
    fb_print(" MiB.\n");

    device_present = 1;
    return 1;
}

uint64_t virtio_blk_capacity_sectors(void) {
    return capacity_sectors;
}

/* Builds a fixed 3-descriptor chain (header -> data -> status), submits
 * it as the single outstanding request, and busy-polls the used ring
 * until the device completes it -- there's never more than one request
 * in flight, so descriptor indices 0/1/2 and avail/used bookkeeping never
 * need to track multiple concurrent requests. */
static int do_request(uint64_t lba, int write) {
    if (!device_present) {
        return -1;
    }

    struct virtio_blk_req_header *hdr =
        (struct virtio_blk_req_header *)(scratch_virt + SCRATCH_HEADER_OFF);
    uint8_t *status = scratch_virt + SCRATCH_STATUS_OFF;
    hdr->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = lba;
    *status = 0xFF; /* poison -- only a real device write of 0 counts as success below */

    desc[0].addr = scratch_phys + SCRATCH_HEADER_OFF;
    desc[0].len = sizeof(struct virtio_blk_req_header);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = scratch_phys + SCRATCH_DATA_OFF;
    desc[1].len = 512;
    desc[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);
    desc[1].next = 2;

    desc[2].addr = scratch_phys + SCRATCH_STATUS_OFF;
    desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next = 0;

    avail->ring[avail->idx % queue_size] = 0;
    asm volatile ("" ::: "memory"); /* descriptor chain must be visible before idx is */
    avail->idx++;
    asm volatile ("" ::: "memory");

    outw(io_base + VIRTIO_REG_QUEUE_NOTIFY, 0);

    while (used->idx == last_used_idx) {
        asm volatile ("pause");
    }
    last_used_idx = used->idx;

    return (*status == 0) ? 0 : -1; /* VIRTIO_BLK_S_OK == 0 */
}

int virtio_blk_read_sector(uint64_t lba, uint8_t *buf) {
    if (do_request(lba, 0) != 0) {
        return -1;
    }
    for (int i = 0; i < 512; i++) {
        buf[i] = scratch_virt[SCRATCH_DATA_OFF + i];
    }
    return 0;
}

int virtio_blk_write_sector(uint64_t lba, const uint8_t *buf) {
    for (int i = 0; i < 512; i++) {
        scratch_virt[SCRATCH_DATA_OFF + i] = buf[i];
    }
    return do_request(lba, 1);
}
