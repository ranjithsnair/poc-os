#include "asm.h"
#include "memlayout.h"
#include "mmu.h"

; Each non-boot CPU ("AP") is started up in response to a STARTUP
; IPI from the boot CPU.  Section B.4.2 of the Multi-Processor
; Specification says that the AP will start in real mode with CS:IP
; set to XY00:0000, where XY is an 8-bit value sent with the
; STARTUP. Thus this code must start at a 4096-byte boundary.
;
; Because this code sets DS to zero, it must sit
; at an address in the low 2^16 bytes.
;
; startothers() (kernel/main.c) sends the STARTUPs one at a time. It
; copies this code (start) to 0x7000. It puts the 64-bit address of a
; newly allocated per-core stack at start-8, the 64-bit address of the
; place to jump to (mpenter) at start-16, and the physical address of
; entrypml4 (only its low 32 bits are ever read - see start32 below,
; physical memory here tops out at PHYSTOP, comfortably under 4GB) at
; start-24.
;
; Like kernel/entry.asm, this code has to take the CPU all the way from
; 16-bit real mode into 64-bit long mode before it can call a real C
; function (mpenter, linked at its normal high KERNBASE address). It
; reuses kernel/entry.asm's entrypml4 boot page table to do so: by the
; time startothers() runs, the BSP has already switched to the real
; kpgdir (main.c's kvmalloc(), called before startothers()), but kpgdir
; has no identity mapping for the low physical addresses this code
; itself runs at, only entrypml4 does (see its own comment in
; kernel/entry.asm) - and kalloc() only ever hands out low, entrypml4-
; mapped pages until kinit2() runs, which main.c deliberately delays
; until after startothers() returns, for exactly this reason.

; This file is linked (see the Makefile) with -N -Ttext 0x7000 and runs
; at exactly that fixed address, so absolute addressing - not the
; RIP-relative addressing BITS 64 defaults to - is what's wanted for
; the [start-N] memory operands in start64 below.
default abs

BITS 16

global start
start:
  cli

  ; Zero data segment registers DS, ES, and SS.
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax

  ; Switch from real to protected mode.  Use a bootstrap GDT that makes
  ; virtual addresses map directly to physical addresses so that the
  ; effective memory map doesn't change during the transition.
  lgdt [gdtdesc]
  mov eax, cr0
  or eax, CR0_PE
  mov cr0, eax

  ; Complete the transition to 32-bit protected mode by using a long jmp
  ; to reload cs and eip.  The segment descriptors are set up with no
  ; translation, so that the mapping is still the identity mapping.
  ; "o32" forces the 32-bit-offset form of the far jump even though we
  ; are still assembling 16-bit code here.
  o32 jmp (1<<3):start32

BITS 32
start32:
  ; Set up the protected-mode data segment registers
  mov ax, (2<<3)            ; Our data segment selector
  mov ds, ax                ; -> DS: Data Segment
  mov es, ax                ; -> ES: Extra Segment
  mov ss, ax                ; -> SS: Stack Segment
  mov ax, 0                 ; Zero segments not ready for use
  mov fs, ax                ; -> FS
  mov gs, ax                ; -> GS

  ; Enable PAE - a long mode prerequisite, same as kernel/entry.asm.
  mov eax, cr4
  or eax, CR4_PAE
  mov cr4, eax

  ; Point CR3 at entrypml4 (see the comment above) - only the low 32
  ; bits of the stashed physical address are needed since cr3 is loaded
  ; through a 32-bit register here, same as kernel/entry.asm.
  mov eax, [start-24]
  mov cr3, eax

  ; Set EFER.LME (long mode enable). Must happen before CR0.PG below,
  ; or the CPU enters legacy PAE paging instead of long mode.
  mov ecx, MSR_EFER
  rdmsr
  or eax, EFER_LME
  wrmsr

  ; Enable paging. The CPU is now in IA-32e compatibility mode: long
  ; mode is architecturally active, but this code is still executing as
  ; 32-bit instructions until the far jump below reloads CS with a
  ; 64-bit code segment.
  mov eax, cr0
  or eax, (CR0_PG | CR0_WP)
  mov cr0, eax

  ; Load a 64-bit-capable GDT and far-jump into its code segment - this
  ; is the instruction that actually switches the CPU into 64-bit
  ; instruction decoding. Unlike kernel/entry.asm, this whole file runs
  ; at the one fixed low address it's linked at (see the Makefile's
  ; -Ttext 0x7000), so a direct jmp here needs no low/high indirection
  ; trick.
  lgdt [start64gdtdesc]
  jmp (1<<3):start64

BITS 64
start64:
  ; Switch to the stack allocated by startothers() and call mpenter(),
  ; both stashed there as plain 64-bit values - no relocation involved,
  ; so a direct indirect call works fine even though mpenter lives at a
  ; high KERNBASE address entrypml4 maps but this code isn't linked at.
  mov rsp, [start-8]
  mov rax, [start-16]
  call rax

  ; mpenter() never returns (mpmain(), which it ends in, is noreturn),
  ; but spin here just in case.
  mov ax, 0x8a00
  mov dx, ax
  out dx, ax
  mov ax, 0x8ae0
  out dx, ax
spin:
  jmp spin

; Bootstrap GDT used only to reach 32-bit protected mode with an
; identity mapping (no translation) from real mode.
align 4
gdt:
  SEG_NULLASM
  SEG_ASM STA_X|STA_R, 0, 0xffffffff
  SEG_ASM STA_W, 0, 0xffffffff

gdtdesc:
  dw (gdtdesc - gdt - 1)
  dd gdt

; Second bootstrap GDT, used only to reach 64-bit long mode - same
; shape as kernel/entry.asm's entry64gdt.
align 16
start64gdt:
  dq 0                   ; null descriptor
  SEG64CODE_ASM           ; flat 64-bit code segment, selector 1<<3
start64gdtdesc:
  dw (start64gdtdesc - start64gdt - 1)
  dd start64gdt
