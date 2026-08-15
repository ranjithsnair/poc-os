; Boot loader, stage 2, BIOS/INT13h variant.
;
; Entered by boot/bootasm_bios.asm (stage 1) via a real-mode far jump,
; DL still holding the boot drive number BIOS gave us at the very
; start. Loads the kernel - a *raw binary* image (kernel.bin, no ELF
; header: see the Makefile's own kernel.bin rule for why the ELF-
; parsing the ATA-PIO path's boot/boot2main.c does isn't needed here -
; and no reason to write that parsing logic in real-mode assembly when
; the build machine can flatten it once, ahead of time, instead), then
; the whole root filesystem image (fs.img), to their real physical
; addresses - both of which are above the 1MB+64KB a real-mode BIOS
; INT13h call can address directly - then switches fully to protected
; mode and jumps to the kernel's real entry point.
;
; KERNEL_LBA/KERNEL_SECTORS/FS_IMG_LBA/KERNEL_ENTRY are generated at
; build time (see the Makefile's bootconfig_bios.h rule) rather than
; hardcoded: the kernel's real size and entry point are properties of
; a specific build, not something this file should have to track by
; hand.

#include "asm.h"
#include "memlayout.h"
#include "mmu.h"
#include "bootconfig_bios.h"

; No ORG: this assembles to a relocatable ELF object - the linker's own
; -Ttext 0x10000 (see the Makefile) places it, the same way
; boot2asm.asm's own build works.
BITS 16

%define STAGE_BUF_SEG   0x5000   ; 0x5000:0 = phys 0x50000, low staging buffer
%define CHUNK_SECTORS   32       ; 32*512=16KB per BIOS call/copy

global entry2
entry2:
  mov [drive], dl

  cli
  xor ax, ax
  mov ds, ax
  mov ss, ax
  ; esp, not sp - see boot/bootasm_bios.asm's own comment on this
  ; exact fix (a 16-bit mov leaves ESP's high half as garbage, which
  ; protected mode's flat 32-bit stack addressing - unlike real mode -
  ; very much cares about).
  mov esp, 0x9000     ; headroom below the 0xA0000 BIOS area
  sti

  call flatten_es

  ; ---- Load the kernel (raw binary) to its real physical load
  ; address (EXTMEM, memlayout.h).
  mov dword [cur_dst], EXTMEM
  mov word [cur_lba], KERNEL_LBA
  mov word [cur_lba+2], 0
  mov word [sectors_left], KERNEL_SECTORS
  call load_chunks

  ; ---- Load the whole root filesystem image to RAMDISK_PADDR - see
  ; kernel/ide.c's own comment for why the kernel needs this already
  ; sitting in RAM rather than ever touching disk hardware itself.
  mov dword [cur_dst], RAMDISK_PADDR
  mov word [cur_lba], FS_IMG_LBA
  mov word [cur_lba+2], 0
  mov word [sectors_left], (RAMDISK_SIZE/512)
  call load_chunks

  ; ---- Full, permanent switch to protected mode - same GDT, same
  ; CR0.PE sequence as boot/bootasm.asm's own (this file's brief
  ; unreal-mode excursions above used the identical GDT already) - and
  ; jump to the kernel's real entry point.
  cli
  lgdt [gdtdesc]
  mov eax, cr0
  or eax, 1
  mov cr0, eax
  jmp (SEG_KCODE<<3):protected_entry

BITS 32
protected_entry:
  mov ax, (SEG_KDATA<<3)
  mov ds, ax
  mov es, ax
  mov ss, ax
  xor ax, ax
  mov fs, ax
  mov gs, ax
  jmp KERNEL_ENTRY

BITS 16
; flatten_es: (re)installs a flat (base 0, 4GB limit) descriptor into
; ES via a brief protected-mode round trip, then drops straight back
; to real mode - see the long comment this used to carry, preserved
; here: a real CPU only reloads a segment's cached descriptor when
; something explicitly MOVs a new selector into it, not just because
; CR0.PE changed, so ES keeps that 4GB limit even after we're back in
; real mode, meaning ES-prefixed 32-bit-offset addressing (es:edi, not
; a real-mode-style 16-bit segment:offset pair) reaches all 4GB from
; here on - while INT13h (real-mode-only) still works normally because
; the CPU itself genuinely is back in real mode. Standard, widely-used
; technique - not specific to this project.
;
; Callable repeatedly, not just once: BIOS's own INT13h implementation
; is free to reload ES for its own scratch use while handling our
; disk-read calls (observed directly - confirmed via QEMU's "info
; registers" that ES's cached base silently reverts to a real-mode-
; style (selector<<4) value, while its limit/access-rights fields stay
; exactly as this routine last left them, after every int 0x13 call).
; Because real-mode segment reloads only ever recompute base (not
; limit/access-rights) from a bare selector value, that leaves ES
; *looking* flat (4GB limit still present) while actually pointing at
; the wrong base - silently corrupting every unreal-mode access after
; the first disk read, hence why this must be re-run before every
; chunk's copy rather than once up front.
flatten_es:
  push eax
  push ebx
  lgdt [gdtdesc]
  mov eax, cr0
  or al, 1
  mov cr0, eax
  jmp $+2                    ; flush the prefetch queue
  mov bx, (SEG_KDATA<<3)
  mov es, bx
  mov eax, cr0
  and al, 0xFE
  mov cr0, eax
  jmp 0:.flush                ; far jump reloads CS, flushing the pipeline
.flush:
  pop ebx
  pop eax
  ret

; load_chunks: reads [sectors_left] sectors from drive [drive],
; starting at LBA [cur_lba], to physical address [cur_dst] -
; CHUNK_SECTORS at a time, via the low (real-mode-reachable) staging
; buffer at STAGE_BUF_SEG, each chunk copied up to its real
; destination with an unreal-mode 32-bit rep movsb. Re-flattens ES
; (see flatten_es above) after every BIOS call and before every copy,
; since the BIOS call itself is what silently invalidates ES.
; Clobbers ax/bx/cx/dx/si/di/es implicitly through the instructions
; below; callers don't rely on any of them surviving.
load_chunks:
.loop:
  cmp word [sectors_left], 0
  je .done

  mov ax, [sectors_left]
  cmp ax, CHUNK_SECTORS
  jbe .have_count
  mov ax, CHUNK_SECTORS
.have_count:
  mov [dap.count], ax

  mov word [dap.seg], STAGE_BUF_SEG
  mov word [dap.off], 0
  mov ax, [cur_lba]
  mov [dap.lba_lo], ax
  mov ax, [cur_lba+2]
  mov [dap.lba_lo+2], ax

  mov dl, [drive]
  mov si, dap
  mov ah, 0x42
  int 0x13
  jc diskfail

  call flatten_es

  ; Copy this chunk from the low staging buffer up to cur_dst.
  movzx ecx, word [dap.count]
  shl ecx, 9                  ; sectors -> bytes (*512)
  xor esi, esi
  mov si, STAGE_BUF_SEG
  shl esi, 4                   ; real-mode segment -> linear address (*16)
  mov edi, [cur_dst]
  a32 rep es movsb

  ; advance cur_lba/cur_dst, decrement sectors_left, by this chunk's
  ; real transfer count (dap.count - the BIOS may have transferred
  ; fewer than requested; re-read it rather than assuming CHUNK_SECTORS).
  mov cx, [dap.count]
  add [cur_lba], cx
  adc word [cur_lba+2], 0
  sub [sectors_left], cx
  movzx ecx, cx
  shl ecx, 9
  add [cur_dst], ecx

  jmp .loop
.done:
  ret

diskfail:
  cli
  hlt
  jmp $

align 4
drive:        db 0
sectors_left: dw 0
cur_lba:      dd 0
cur_dst:      dd 0

; Disk Address Packet for INT13h AH=0x42 (16 bytes) - see
; boot/bootasm_bios.asm's own copy for the field layout.
align 4
dap:
  db 0x10
  db 0
.count: dw 0
.off:   dw 0
.seg:   dw 0
.lba_lo: dd 0
         dd 0

; Bootstrap GDT - identical in shape to boot/bootasm.asm's own (flat,
; identity-mapped code/data descriptors): used both for the repeated
; unreal-mode excursions above and the final real switch to protected
; mode.
align 4
gdt:
  SEG_NULLASM
  SEG_ASM STA_X|STA_R, 0x0, 0xffffffff
  SEG_ASM STA_W, 0x0, 0xffffffff

gdtdesc:
  dw (gdtdesc - gdt - 1)
  dd gdt
