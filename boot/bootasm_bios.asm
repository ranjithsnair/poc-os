; Boot loader, stage 1, BIOS/INT13h variant.
;
; The real-hardware/AHCI-SATA/USB-boot/VirtualBox-as-DVD/El-Torito-CD
; counterpart to boot/bootasm.asm+boot/bootmain.c (which only work
; against a real/emulated legacy IDE controller - see that pair's own
; comments). Never leaves 16-bit real mode at all (unlike bootasm.asm,
; which switches to protected mode immediately): BIOS INT13h - the one
; disk-reading mechanism real firmware already implements correctly
; for every one of those cases - only works in real mode, so both this
; stage and stage 2 (boot/boot2_bios.asm) stay in real mode for as
; long as they still need to read more disk, only switching to
; protected mode at the very end of stage 2, right before jumping to
; the kernel.
;
; Reads via CHS (AH=0x02), not INT13h extensions (AH=0x42/LBA): this
; image needs to boot identically from a plain USB/hard-disk-style
; drive (real hardware, VirtualBox, QEMU as a raw disk) *and* from an
; El-Torito "hard disk emulation" CD-ROM (for a single ISO that also
; works when burned to disc or attached as a virtual CD) - and, found
; the hard way, El Torito hard-disk-emulation drives commonly fail the
; INT13h-extensions-present check (AH=0x41) outright, while the CD's
; own *native* drive number (the "no emulation" El Torito path) passes
; that check but then hangs on the actual AH=0x42 read (a QEMU/SeaBIOS
; ATAPI-sector-size-vs-DAP-LBA-units mismatch, near as can be told).
; CHS (AH=0x02) sidesteps both: it's the *original* INT13h interface,
; universally implemented (real BIOS, VirtualBox, QEMU) since long
; before LBA extensions existed, and El Torito hard-disk emulation is
; specifically designed to synthesize a CHS geometry a plain BIOS
; bootloader can address correctly - it's exactly how GRUB Legacy/
; isolinux/syslinux all boot from El-Torito CDs to this day. Geometry
; (sectors/track, heads) comes from AH=0x08 on the actual boot drive,
; not a hardcoded guess: real drives (and CD emulation layers) don't
; all agree on one geometry, and getting it wrong silently reads the
; wrong sectors instead of failing loudly.
;
; Loads stage 2 (STAGE2_SECTORS sectors starting at LBA 1) to
; STAGE2_ADDR one sector at a time, then far-jumps to it - still in
; real mode, DL (the boot drive number BIOS put there before ever
; handing control to us) untouched and still valid for stage 2 to
; read.
;
; STAGE2_ADDR is 0x1000 (4KB physical), not 0x10000 like the ATA-PIO
; path's stage 2 (boot/bootmain.c's STAGE2_ADDR): this stage 2 (boot/
; boot2_bios.asm) is genuine 16-bit real-mode code, and GNU ld's 16-bit
; ELF relocations for same-segment label references can't represent an
; address >= 0x10000 (65536) at all - "relocation truncated to fit"
; found the hard way. 0x1000 sits safely below both that ceiling and
; the boot sector's own 0x7C00 load address/stack; STAGE2_SECTORS is
; also far smaller here than the ATA-PIO path's 256 (this stage 2 is
; hand-written assembly with no ELF-parsing logic at all - see boot/
; boot2_bios.asm's own comment for why - so it doesn't need nearly as
; much room), keeping stage 2's own end (0x1000 + STAGE2_SECTORS*512)
; comfortably clear of 0x7C00 too.

#include "memlayout.h"

; No ORG: this assembles to a relocatable ELF object, like every other
; file under boot/ - the linker's own -Ttext 0x7C00 (see the Makefile)
; places it, the same way bootasm.asm's own build works.
BITS 16

%define STAGE2_OFF     0x1000
%define STAGE2_SECTORS 16

global start
start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  ; esp, not sp: a 16-bit mov only touches ESP's low half, leaving
  ; whatever garbage was in its high half (BIOS's own, before handing
  ; control to us) in place - invisible in real mode (nothing but the
  ; low 16 bits is ever addressed there), but very much not once
  ; protected mode's flat 32-bit segments make the *whole* register
  ; the effective stack pointer (found this the hard way: a huge,
  ; garbage ESP straight out of "mov sp" caused a stack-corruption
  ; crash right after the final switch to protected mode in boot/
  ; boot2_bios.asm, which reuses this same stack).
  mov esp, 0x7C00

  mov [drive], dl

  ; Physical address line A20 is tied to zero by default (so the first
  ; PCs with 2MB would run software that assumed 1MB) - undo that
  ; *before* anything here or in stage 2 ever touches memory above
  ; 1MB (the kernel at EXTMEM, the ramdisk at RAMDISK_PADDR - both
  ; miss this if A20 stays masked: bit 20 of the address gets dropped,
  ; silently aliasing every >1MB access down into low memory instead).
  ; Same sequence boot/bootasm.asm's own A20 comment uses.
seta20.1:
  in al, 0x64
  test al, 0x2
  jnz seta20.1
  mov al, 0xd1
  out 0x64, al
seta20.2:
  in al, 0x64
  test al, 0x2
  jnz seta20.2
  mov al, 0xdf
  out 0x60, al

  sti                     ; BIOS calls expect interrupts enabled

  call get_geometry

  mov word [sectors_left], STAGE2_SECTORS
  mov dword [cur_lba], 1
  mov word [cur_seg], 0
  mov word [cur_off], STAGE2_OFF

.loop:
  cmp word [sectors_left], 0
  je done

  mov ax, [cur_seg]
  mov es, ax
  mov bx, [cur_off]
  call read_sector

  add word [cur_off], 512
  inc dword [cur_lba]
  dec word [sectors_left]
  jmp .loop

done:
  ; Far jump into stage 2, still in real mode - see boot/boot2_bios.asm.
  jmp 0x0000:STAGE2_OFF

; get_geometry: queries the boot drive's CHS geometry via INT13h
; AH=0x08, storing sectors-per-track and head-count for lba_to_chs
; below. Halts on failure - nothing sensible to do without it.
get_geometry:
  push es
  push di
  xor ax, ax
  mov es, ax
  mov di, ax              ; ES:DI = 0:0 - some BIOSes misbehave otherwise
  mov ah, 0x08
  mov dl, [drive]
  int 0x13
  jc geomfail
  and cl, 0x3F             ; sectors/track = CL bits 0-5
  mov [spt], cl
  movzx ax, dh
  inc ax                    ; heads = DH + 1 (DH is the max head *number*) -
                              ; a WORD, not a byte: DH=0xFF (max legal value,
                              ; the standard "large disk" 256-head geometry)
                              ; gives heads=256, which doesn't fit in 8 bits -
                              ; found via a real divide-by-zero crash when
                              ; this wrapped to 0 in a byte-sized field.
  mov [heads], ax
  pop di
  pop es
  ret
geomfail:
  cli
  hlt
  jmp $

; lba_to_chs: converts DWORD [cur_lba] into cylinder/head/sector using
; [spt]/[heads] (get_geometry above). Sector is 1-based, per INT13h
; convention.
lba_to_chs:
  xor edx, edx
  mov eax, [cur_lba]
  movzx ecx, byte [spt]
  div ecx                   ; eax = lba/spt, edx = lba%spt
  inc edx
  mov [chs_sector], dl
  xor edx, edx
  movzx ecx, word [heads]
  div ecx                   ; eax = cylinder, edx = head
  mov [chs_cyl], ax
  mov [chs_head], dl
  ret

; read_sector: reads the single sector at DWORD [cur_lba] from drive
; [drive] into ES:BX, retrying (with a controller reset) a few times
; before giving up - real drive controllers occasionally need this.
; Clobbers ax/bx/cx/dx.
read_sector:
  call lba_to_chs
  mov cx, [chs_cyl]
  mov ch, cl                ; cylinder low 8 bits -> CH
  mov cl, [chs_cyl+1]
  shl cl, 6                 ; cylinder high 2 bits -> CL bits 6-7
  or cl, [chs_sector]        ; sector (1-63) -> CL bits 0-5
  mov dh, [chs_head]
  mov dl, [drive]
  mov ax, 0x0201             ; AH=0x02 (read), AL=1 sector
  int 0x13
  jnc .ok
  ; Reset the controller and retry once before failing loudly.
  xor ax, ax
  mov dl, [drive]
  int 0x13
  mov cx, [chs_cyl]
  mov ch, cl
  mov cl, [chs_cyl+1]
  shl cl, 6
  or cl, [chs_sector]
  mov dh, [chs_head]
  mov dl, [drive]
  mov ax, 0x0201
  int 0x13
  jc diskfail
.ok:
  ret

diskfail:
  cli
  hlt
  jmp $

align 4
drive:        db 0
sectors_left: dw 0
cur_lba:      dd 0
cur_seg:      dw 0
cur_off:      dw 0
spt:          db 0
heads:        dw 0
chs_cyl:      dw 0
chs_head:     db 0
chs_sector:   db 0

; MBR partition table (offset 0x1BE/446, four 16-byte entries, ending
; at 0x1FE/510 where the 0x55AA signature goes - sign.pl appends that
; itself, matching every other file under boot/). Not needed for our
; own boot process (stage 1/2 read the whole image directly by LBA, no
; partition parsing) - added because BIOS's El-Torito "hard disk
; emulation" CD-boot path was found (the hard way) to synthesize its
; reported CHS geometry by reading *this* table, and returns garbage
; (spt=0) via get_geometry's own INT13h AH=0x08 call without one -
; which then divide-by-zero-crashes lba_to_chs. One entry, marked
; active/bootable, spanning the whole image; CHS fields use the
; standard 0xFE/0xFF/0xFF "overflow, use the LBA fields instead"
; convention every MBR partitioning tool has used for decades, since
; our image doesn't fit any small, exact CHS geometry. 15000 must
; match the Makefile's poc_bios.img rule (dd if=/dev/zero count=15000)
; - the two aren't otherwise connected, so if one changes, so must the
; other.
times (0x1BE - ($-$$)) db 0
db 0x80                     ; boot indicator: active/bootable
db 0xFF, 0xFF, 0xFE          ; start CHS: overflow marker (start LBA 0)
db 0x0C                     ; partition type: FAT32 LBA (arbitrary/unused)
db 0xFF, 0xFF, 0xFE          ; end CHS: overflow marker
dd 0                         ; start LBA
dd 15000                     ; number of sectors - must match poc_bios.img's size
times (0x1FE - ($-$$)) db 0   ; three remaining (empty) partition entries

; No self-padding/signature here - boot/sign.pl (see the Makefile's
; bootblock_bios rule) pads the extracted raw binary to 510 bytes and
; appends the real 0x55AA signature itself, exactly like it already
; does for boot/bootasm.asm+boot/bootmain.c.
