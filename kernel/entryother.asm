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
; Startothers (in main.c) sends the STARTUPs one at a time.
; It copies this code (start) at 0x7000.  It puts the address of
; a newly allocated per-core stack in start-4, the address of the
; place to jump to (mpenter) in start-8, and the physical address
; of entrypgdir in start-12.
;
; This code combines elements of bootasm.asm and entry.asm.

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
  o32 jmp (SEG_KCODE<<3):start32

BITS 32
start32:
  ; Set up the protected-mode data segment registers
  mov ax, (SEG_KDATA<<3)   ; Our data segment selector
  mov ds, ax               ; -> DS: Data Segment
  mov es, ax               ; -> ES: Extra Segment
  mov ss, ax               ; -> SS: Stack Segment
  mov ax, 0                ; Zero segments not ready for use
  mov fs, ax               ; -> FS
  mov gs, ax               ; -> GS

  ; Turn on page size extension for 4Mbyte pages
  mov eax, cr4
  or eax, (CR4_PSE)
  mov cr4, eax
  ; Use entrypgdir as our initial page table
  mov eax, [start-12]
  mov cr3, eax
  ; Turn on paging.
  mov eax, cr0
  or eax, (CR0_PE|CR0_PG|CR0_WP)
  mov cr0, eax

  ; Switch to the stack allocated by startothers()
  mov esp, [start-4]
  ; Call mpenter()
  call [start-8]

  mov ax, 0x8a00
  mov dx, ax
  out dx, ax
  mov ax, 0x8ae0
  out dx, ax
spin:
  jmp spin

align 4
gdt:
  SEG_NULLASM
  SEG_ASM STA_X|STA_R, 0, 0xffffffff
  SEG_ASM STA_W, 0, 0xffffffff

gdtdesc:
  dw (gdtdesc - gdt - 1)
  dd gdt
