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
; Reads via CHS (AH=0x02), not INT13h extensions (AH=0x42/LBA) - see
; boot/bootasm_bios.asm's own comment for why: this same image has to
; boot from a plain USB/hard-disk-style drive *and* from an El-Torito
; "hard disk emulation" CD-ROM (for one ISO that also works burned to
; disc or attached as a virtual CD), and El-Torito hard-disk-emulation
; drives were found to fail the INT13h-extensions-present check
; outright, while the CD's own native drive number passes that check
; but then hangs on the actual extended read. CHS sidesteps both.
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
; -Ttext 0x1000 (see the Makefile) places it, the same way
; boot2asm.asm's own build works.
BITS 16

%define STAGE_BUF_SEG   0x5000   ; 0x5000:0 = phys 0x50000, low staging buffer
%define CHUNK_SECTORS   32       ; 32*512=16KB per copy (read one CHS sector
                                  ; at a time within the chunk - see read_sector)
%define VBE_CTRLBUF (VBE_INFO_PADDR+0x100)   ; transient VBE Controller Info buffer
%define VBE_MODEBUF (VBE_INFO_PADDR+0x300)   ; transient VBE Mode Info buffer

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
  call get_geometry

  ; ---- Load the kernel (raw binary) to its real physical load
  ; address (EXTMEM, memlayout.h).
  mov dword [cur_dst], EXTMEM
  mov dword [cur_lba], KERNEL_LBA
  mov word [sectors_left], KERNEL_SECTORS
  call load_chunks

  ; ---- Load the whole root filesystem image to RAMDISK_PADDR - see
  ; kernel/ide.c's own comment for why the kernel needs this already
  ; sitting in RAM rather than ever touching disk hardware itself.
  mov dword [cur_dst], RAMDISK_PADDR
  mov dword [cur_lba], FS_IMG_LBA
  mov word [sectors_left], (RAMDISK_SIZE/512)
  call load_chunks

  ; ---- Probe for a usable VBE linear-framebuffer mode and set it, if
  ; found - see setup_vbe's own comment. Purely optional: any failure
  ; just leaves VBE_INFO_PADDR unwritten and boot proceeds exactly as
  ; it always has, straight into the protected-mode switch below.
  call setup_vbe

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
; to real mode. A real CPU only reloads a segment's cached descriptor
; when something explicitly MOVs a new selector into it, not just
; because CR0.PE changed, so ES keeps that 4GB limit even after we're
; back in real mode, meaning ES-prefixed 32-bit-offset addressing
; (es:edi, not a real-mode-style 16-bit segment:offset pair) reaches
; all 4GB from here on - while INT13h (real-mode-only) still works
; normally because the CPU itself genuinely is back in real mode.
; Standard, widely-used technique - not specific to this project.
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

; setup_vbe: probes for a usable VBE (VESA BIOS Extensions) 32-bit-per-
; pixel linear-framebuffer mode and, if one is found, sets it - real
; mode only, same as every other routine in this file, called from
; entry2 right before the final protected-mode switch. See
; include/vbe.h and include/memlayout.h's VBE_INFO_PADDR/VBE_INFO_MAGIC
; for the struct this writes and where.
;
; The fixed VESA "standard mode number" table (0x100-0x11B) tops out at
; 24bpp - a real 32bpp linear-framebuffer mode exists on QEMU stdvga,
; VirtualBox's VBE BIOS, and real VBE 2.0+ hardware alike, but only
; under an OEM-specific mode number reachable by walking the BIOS's own
; supported-mode list (VideoModePtr, from "Get Controller Info" below),
; not the fixed table - hence the enumeration this routine does rather
; than trying a short fixed list directly.
;
; Preference order 1024x768 -> 800x600 -> 640x480 (all near-universally
; available on QEMU/VirtualBox/real VBE 2.0+ hardware) is implemented
; as a single scan assigning each 32bpp-direct-color-LFB candidate a
; preference rank and keeping the highest-ranked match seen so far -
; not a separate pass per resolution.
;
; Clobbers ax/bx/cx/dx/si/di/es, same "no save/restore, nothing after
; the call site depends on register state" convention load_chunks/
; read_sector/etc already use in this file. Every failure path (VBE
; unsupported at all, no matching mode found, Set Mode itself failing)
; falls through to .done without ever writing VBE_INFO_MAGIC - boot
; proceeds normally either way, see this routine's call site in entry2.
setup_vbe:
  ; ---- Get Controller Info (AX=4F00). ES:DI -> VBE_CTRLBUF (a
  ; 512-byte buffer), pre-seeded with the "VBE2" signature to request
  ; VBE 2.0+ fields (standard practice, not required by every BIOS but
  ; harmless and costs nothing).
  xor ax, ax
  mov es, ax
  mov di, VBE_CTRLBUF
  mov dword [VBE_CTRLBUF], 0x32454256 ; 'VBE2' little-endian
  mov ax, 0x4F00
  int 0x10
  cmp ax, 0x004F
  jne .done                           ; no VBE at all

  ; VideoModePtr (offset word at +14, segment word at +16) - DS=0
  ; throughout this file (entry2's own prologue), so the data this call
  ; just wrote is readable via plain DS-relative addressing with no es:
  ; prefix, the same as every other fixed-address buffer here; only the
  ; call itself needed an explicit ES:DI input pointer.
  mov ax, [VBE_CTRLBUF+14]
  mov [modelist_off], ax
  mov ax, [VBE_CTRLBUF+16]
  mov [modelist_seg], ax

  mov word [best_mode], 0
  mov word [best_rank], 0

.scan_loop:
  mov ax, [modelist_seg]
  mov es, ax
  mov si, [modelist_off]
  mov ax, [es:si]                     ; next mode number in the list
  cmp ax, 0xFFFF
  je .scan_done
  mov [cur_modenum], ax
  add word [modelist_off], 2

  ; Get Mode Info (AX=4F01) for this candidate. ES:DI -> VBE_MODEBUF (a
  ; 256-byte buffer).
  xor bx, bx
  mov es, bx
  mov di, VBE_MODEBUF
  mov ax, 0x4F01
  mov cx, [cur_modenum]
  int 0x10
  cmp ax, 0x004F
  jne .scan_loop                      ; this candidate failed the query

  ; ModeAttributes (word @+0): need bit0 (supported) and bit7 (linear
  ; framebuffer available, VBE 2.0+).
  mov ax, [VBE_MODEBUF+0]
  test ax, 0x0001
  jz .scan_loop
  test ax, 0x0080
  jz .scan_loop
  ; MemoryModel (byte @+27): need 6 (direct color).
  mov al, [VBE_MODEBUF+27]
  cmp al, 6
  jne .scan_loop
  ; BitsPerPixel (byte @+25): need 32.
  mov al, [VBE_MODEBUF+25]
  cmp al, 32
  jne .scan_loop

  ; Resolution -> preference rank (higher wins; a later same-rank
  ; candidate never replaces an earlier one - see .have_rank below).
  mov ax, [VBE_MODEBUF+18]            ; XResolution
  mov bx, [VBE_MODEBUF+20]            ; YResolution
  cmp ax, 1024
  jne .not1024
  cmp bx, 768
  jne .not1024
  mov cx, 3
  jmp .have_rank
.not1024:
  cmp ax, 800
  jne .not800
  cmp bx, 600
  jne .not800
  mov cx, 2
  jmp .have_rank
.not800:
  cmp ax, 640
  jne .scan_loop
  cmp bx, 480
  jne .scan_loop
  mov cx, 1
.have_rank:
  cmp cx, [best_rank]
  jle .scan_loop
  mov [best_rank], cx
  mov ax, [cur_modenum]
  mov [best_mode], ax
  jmp .scan_loop

.scan_done:
  cmp word [best_mode], 0
  je .done                            ; enumerated fine, nothing usable found

  ; ---- Set VBE Mode (AX=4F02). BX = mode | 0x4000 (bit14: use the
  ; linear/flat framebuffer model, not the legacy banked-window one).
  mov ax, 0x4F02
  mov bx, [best_mode]
  or bx, 0x4000
  int 0x10
  cmp ax, 0x004F
  jne .done                           ; Set Mode itself failed

  ; ---- Re-query the chosen mode's ModeInfoBlock - guaranteed to
  ; already be sitting at VBE_MODEBUF from .scan_loop's last successful
  ; Get Mode Info call on best_mode, but a fresh, defensive re-query
  ; costs nothing and removes any assumption that the scan loop's
  ; control flow reaches here with VBE_MODEBUF still valid.
  xor bx, bx
  mov es, bx
  mov di, VBE_MODEBUF
  mov ax, 0x4F01
  mov cx, [best_mode]
  int 0x10
  cmp ax, 0x004F
  jne .done

  ; ---- Copy the fields this driver needs into the persistent struct
  ; at VBE_INFO_PADDR - see include/vbe.h's struct vbeinfo for the
  ; exact byte layout this must match. magic is written last, only
  ; once every step above has actually succeeded.
  mov eax, [VBE_MODEBUF+40]           ; PhysBasePtr
  mov [VBE_INFO_PADDR+4], eax
  movzx eax, word [VBE_MODEBUF+16]    ; BytesPerScanLine
  mov [VBE_INFO_PADDR+8], eax
  movzx eax, word [VBE_MODEBUF+18]    ; XResolution
  mov [VBE_INFO_PADDR+12], eax
  movzx eax, word [VBE_MODEBUF+20]    ; YResolution
  mov [VBE_INFO_PADDR+16], eax
  mov al, [VBE_MODEBUF+25]            ; BitsPerPixel
  mov [VBE_INFO_PADDR+20], al
  mov al, [VBE_MODEBUF+31]            ; RedMaskSize
  mov [VBE_INFO_PADDR+21], al
  mov al, [VBE_MODEBUF+32]            ; RedFieldPosition
  mov [VBE_INFO_PADDR+22], al
  mov al, [VBE_MODEBUF+33]            ; GreenMaskSize
  mov [VBE_INFO_PADDR+23], al
  mov al, [VBE_MODEBUF+34]            ; GreenFieldPosition
  mov [VBE_INFO_PADDR+24], al
  mov al, [VBE_MODEBUF+35]            ; BlueMaskSize
  mov [VBE_INFO_PADDR+25], al
  mov al, [VBE_MODEBUF+36]            ; BlueFieldPosition
  mov [VBE_INFO_PADDR+26], al
  mov byte [VBE_INFO_PADDR+27], 0     ; reserved pad

  mov dword [VBE_INFO_PADDR], VBE_INFO_MAGIC   ; written LAST

.done:
  ret

align 4
modelist_off: dw 0
modelist_seg: dw 0
cur_modenum:  dw 0
best_mode:    dw 0
best_rank:    dw 0

; lba_to_chs: converts DWORD [cur_sec_lba] into cylinder/head/sector
; using [spt]/[heads] (get_geometry above). Sector is 1-based, per
; INT13h convention.
lba_to_chs:
  xor edx, edx
  mov eax, [cur_sec_lba]
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

; read_sectors: reads [read_count] sectors (1..spt - caller guarantees
; this never crosses a track boundary, see load_chunks' batching)
; starting at DWORD [cur_sec_lba] from drive [drive] into ES:BX,
; retrying (with a controller reset) once before giving up. Clobbers
; ax/bx/cx/dx.
read_sectors:
  call lba_to_chs
  mov cx, [chs_cyl]
  mov ch, cl
  mov cl, [chs_cyl+1]
  shl cl, 6
  or cl, [chs_sector]
  mov dh, [chs_head]
  mov dl, [drive]
  mov al, [read_count]
  mov ah, 0x02
  int 0x13
  jnc .ok
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
  mov al, [read_count]
  mov ah, 0x02
  int 0x13
  jc diskfail
.ok:
  ret

; load_chunks: reads [sectors_left] sectors from drive [drive],
; starting at LBA [cur_lba], to physical address [cur_dst] -
; CHUNK_SECTORS at a time, via the low (real-mode-reachable) staging
; buffer at STAGE_BUF_SEG (one CHS sector per INT13h call - see
; read_sector), each chunk then copied up to its real destination
; with an unreal-mode 32-bit rep movsb. Re-flattens ES (see
; flatten_es above) after every BIOS call and before every copy,
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
  mov [chunk_count], ax

  ; Read this chunk's sectors into the staging buffer at
  ; STAGE_BUF_SEG:0, STAGE_BUF_SEG:0x200, etc. - batched into as few
  ; INT13h AH=0x02 calls as the current track allows (never crossing a
  ; track boundary within one call, the safe/universal convention for
  ; multi-sector CHS reads), rather than one call per sector: with
  ; RAMDISK_SIZE grown past ~2MB/4000 sectors (see its own comment),
  ; one-call-per-sector made the ramdisk load alone take minutes under
  ; QEMU/SeaBIOS's per-INT13h-call emulation overhead.
  mov eax, [cur_lba]
  mov [cur_sec_lba], eax
  mov word [stage_off], 0
  mov cx, [chunk_count]     ; cx = sectors remaining in this chunk
.readloop:
  cmp cx, 0
  je .readdone

  ; count = min(cx, sectors remaining in the current track)
  push cx
  mov eax, [cur_sec_lba]
  xor edx, edx
  movzx ebx, byte [spt]
  div ebx                   ; edx = cur_sec_lba % spt (0-based sector-in-track)
  movzx eax, byte [spt]
  sub eax, edx               ; eax = sectors left in this track
  pop cx
  cmp ax, cx
  jbe .have_batch
  mov ax, cx
.have_batch:
  mov [read_count], al

  mov ax, STAGE_BUF_SEG
  mov es, ax
  mov bx, [stage_off]
  push cx
  call read_sectors
  pop cx

  movzx ax, byte [read_count]
  mov dx, ax
  shl dx, 9
  add word [stage_off], dx
  movzx edx, byte [read_count]
  add [cur_sec_lba], edx
  sub cx, ax
  jmp .readloop
.readdone:

  call flatten_es

  ; Copy this chunk from the low staging buffer up to cur_dst.
  movzx ecx, word [chunk_count]
  shl ecx, 9                  ; sectors -> bytes (*512)
  xor esi, esi
  mov si, STAGE_BUF_SEG
  shl esi, 4                   ; real-mode segment -> linear address (*16)
  mov edi, [cur_dst]
  a32 rep es movsb

  ; advance cur_lba/cur_dst, decrement sectors_left, by this chunk's
  ; sector count.
  mov cx, [chunk_count]
  add [cur_lba], cx
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
chunk_count:  dw 0
stage_off:    dw 0
cur_sec_lba:  dd 0
spt:          db 0
heads:        dw 0
chs_cyl:      dw 0
chs_head:     db 0
chs_sector:   db 0
read_count:   db 0

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
