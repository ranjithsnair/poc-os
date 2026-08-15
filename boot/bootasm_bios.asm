; Boot loader, stage 1, BIOS/INT13h variant.
;
; The real-hardware/AHCI-SATA/USB-boot/VirtualBox-as-DVD counterpart to
; boot/bootasm.asm+boot/bootmain.c (which only work against a real/
; emulated legacy IDE controller - see that pair's own comments).
; Never leaves 16-bit real mode at all (unlike bootasm.asm, which
; switches to protected mode immediately): BIOS INT13h - the one disk-
; reading mechanism real firmware already implements correctly for
; every one of those cases, since it's exactly what every other legacy
; MBR bootloader relies on too - only works in real mode, so both this
; stage and stage 2 (boot/boot2_bios.asm) stay in real mode for as long
; as they still need to read more disk, only switching to protected
; mode at the very end of stage 2, right before jumping to the kernel.
;
; Loads stage 2 (STAGE2_SECTORS sectors starting at LBA 1) to
; STAGE2_ADDR via INT13h extended reads, chunked (CHUNK_SECTORS per
; BIOS call) since not every BIOS implementation reliably supports a
; single huge transfer, then far-jumps to it - still in real mode, DL
; (the boot drive number BIOS put there before ever handing control to
; us) untouched and still valid for stage 2 to read.
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
; comfortably clear of 0x7C00 too. Small enough that the whole
; transfer fits in one 16-bit segment's offset range (0x1000..0x3000),
; so the DAP's segment stays fixed at 0 throughout - only its offset
; field advances each chunk.

#include "memlayout.h"

; No ORG: this assembles to a relocatable ELF object, like every other
; file under boot/ - the linker's own -Ttext 0x7C00 (see the Makefile)
; places it, the same way bootasm.asm's own build works.
BITS 16

%define STAGE2_OFF     0x1000
%define STAGE2_SECTORS 16
%define CHUNK_SECTORS  16      ; conservative per-INT13h-call transfer size

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

  ; Check INT13h extensions (LBA reads) are actually present on this
  ; drive before ever relying on them - AH=0x41, BX=0x55AA in, CF=0/
  ; BX=0xAA55 out on success. Every BIOS written in the last ~25 years
  ; supports this on a boot drive (it's how El Torito hard-disk-
  ; emulation and every USB/SATA boot path already works), but failing
  ; loudly (park in checkfail below) beats silently misreading disk.
  mov ah, 0x41
  mov bx, 0x55AA
  mov dl, [drive]
  int 0x13
  jc checkfail
  cmp bx, 0xAA55
  jne checkfail

  mov word [sectors_left], STAGE2_SECTORS
  mov word [cur_lba], 1
  mov word [cur_lba+2], 0
  mov word [cur_off], STAGE2_OFF

.loop:
  cmp word [sectors_left], 0
  je done

  mov ax, [sectors_left]
  cmp ax, CHUNK_SECTORS
  jbe .have_count
  mov ax, CHUNK_SECTORS
.have_count:
  mov [dap.count], ax

  mov word [dap.seg], 0
  mov ax, [cur_off]
  mov [dap.off], ax

  mov ax, [cur_lba]
  mov [dap.lba_lo], ax
  mov ax, [cur_lba+2]
  mov [dap.lba_lo+2], ax

  mov dl, [drive]
  mov si, dap
  mov ah, 0x42
  int 0x13
  jc diskfail

  ; advance: cur_lba += count, cur_off += count*512, sectors_left -= count
  mov cx, [dap.count]
  add [cur_lba], cx
  adc word [cur_lba+2], 0
  sub [sectors_left], cx
  mov ax, cx
  shl ax, 9
  add [cur_off], ax

  jmp .loop

done:
  ; Far jump into stage 2, still in real mode - see boot/boot2_bios.asm.
  jmp 0x0000:STAGE2_OFF

checkfail:
  cli
  hlt
  jmp $

diskfail:
  cli
  hlt
  jmp $

align 4
drive:        db 0
sectors_left: dw 0
cur_lba:      dd 0
cur_off:      dw 0

; Disk Address Packet for INT13h AH=0x42 (16 bytes).
align 4
dap:
  db 0x10          ; packet size
  db 0              ; reserved
.count: dw 0
.off:   dw 0
.seg:   dw 0
.lba_lo: dd 0
         dd 0       ; LBA high 32 bits - always 0, no disk here is anywhere near 2TB

; No self-padding/signature here - boot/sign.pl (see the Makefile's
; bootblock_bios rule) pads the extracted raw binary to 510 bytes and
; appends the real 0x55AA signature itself, exactly like it already
; does for boot/bootasm.asm+boot/bootmain.c.
