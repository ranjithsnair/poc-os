; The poc kernel starts executing in this file (64-bit build). This file
; is linked with the kernel C code, so it can refer to kernel symbols
; such as main(). The boot block (boot/bootasm.asm and boot/bootmain.c) -
; which never leaves 32-bit protected mode itself, see the Makefile's
; BOOTCC - jumps to entry below.
;
; Unlike entry.asm (32-bit build), this file's job is bigger than just
; turning on paging: it has to take the CPU from 32-bit protected mode
; all the way into 64-bit long mode before it can jump to main() at its
; canonical high virtual address. In order:
;   1. build a boot-time PML4/PDPT/PD (4 levels, but using 2MB pages so
;      no PT level is needed yet) that both identity-maps low memory and
;      maps it again at KERNBASE - see the comment above entry, below,
;      for why both are needed
;   2. enable PAE (CR4.PAE) - a prerequisite for long mode
;   3. point CR3 at the PML4
;   4. set IA32_EFER.LME (long mode enable) via wrmsr
;   5. enable paging (CR0.PG) - the CPU is now in "compatibility mode":
;      long mode is architecturally active, but this code is still
;      executing as 32-bit instructions
;   6. load a GDT with a 64-bit (L-bit) code segment and far-jump into
;      it - this is what actually switches the CPU to 64-bit mode
;   7. only now can 64-bit instructions (a real 64-bit stack, a 64-bit
;      main() pointer, etc.) be used

#include "asm.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"

BITS 32

extern main

; Multiboot header, for multiboot boot loaders like GNU Grub. Unused by
; poc's own bootasm.asm/bootmain.c; kept only for parity with entry.asm
; (see the 32-bit build for the long-form comment on why it's here).
align 4
section .text
global multiboot_header
multiboot_header:
  %define magic 0x1badb002
  %define flags 0
  dd magic
  dd flags
  dd (-magic-flags)

; By convention, the _start symbol specifies the ELF entry point. Since
; we haven't set up virtual memory yet, our entry point is the physical
; address of 'entry'.
global _start
_start equ V2P_WO(entry)

; Entering poc on boot processor: bootasm.asm has already done
; real->protected mode; paging is still off.
global entry
entry:
  ; Build the boot-time page tables (entrypd/entrypdpt_low/
  ; entrypdpt_high/entrypml4, all declared below), all still
  ; physical-address writes since paging is off. Only two mappings are
  ; installed, both covering physical [0,4MB) with 2MB pages:
  ;   - identity (VA [0,4MB) -> PA [0,4MB)): needed because the very
  ;     next instruction after paging turns on is still executing at
  ;     its low physical address (eip hasn't moved) - that address has
  ;     to keep translating to itself or the CPU faults immediately.
  ;   - high (VA [KERNBASE,KERNBASE+4MB) -> PA [0,4MB)): where the
  ;     kernel actually expects to run once it reaches main() below.
  ; entrypdpt_high/entrypml4 only need one entry each installed (at
  ; index PDPTX(KERNBASE)=510 and PML4X(KERNBASE)=511 respectively,
  ; both compile-time constants since KERNBASE is a literal) since
  ; KERNBASE's own address already picks out those slots; entrypml4[0]
  ; and entrypdpt_low[0] cover the identity mapping the same way. Both
  ; paths share the same entrypd table - a PD entry doesn't care which
  ; PML4/PDPT slots were used to reach it.

  ; entrypd[0] = 0x000000 | P|W|PS,  entrypd[1] = 0x200000 | P|W|PS
  mov dword [V2P_WO(entrypd) + 0*8], (0x000000 | PTE_P | PTE_W | PTE_PS)
  mov dword [V2P_WO(entrypd) + 1*8], (0x200000 | PTE_P | PTE_W | PTE_PS)

  ; entrypdpt_low[0] = entrypdpt_high[PDPTX(KERNBASE)] = entrypd | P|W
  ;
  ; PDPTX(KERNBASE)/PML4X(KERNBASE) below are written as plain numbers
  ; (510 and 511) rather than the mmu.h macros of the same name: those
  ; macros contain a C-style (uintp) cast, which NASM's expression
  ; parser doesn't understand (this file only goes through the C
  ; *preprocessor* - #define expansion - not the C compiler proper, see
  ; the Makefile's kernel/%.asm rule). 510/511 are just PDPTX/PML4X
  ; hand-evaluated for the fixed KERNBASE literal: KERNBASE's bits
  ; 63:31 are all 1 and bits 30:0 are all 0, so PML4X (bits 47:39) is
  ; 0x1FF=511, and PDPTX (bits 38:30) is bit30=0 followed by eight 1s -
  ; 0b111111110 = 0x1FE = 510.
  mov eax, V2P_WO(entrypd)
  or eax, (PTE_P | PTE_W)
  mov dword [V2P_WO(entrypdpt_low) + 0*8], eax
  mov dword [V2P_WO(entrypdpt_high) + 510*8], eax

  ; entrypml4[0] = entrypdpt_low | P|W
  mov eax, V2P_WO(entrypdpt_low)
  or eax, (PTE_P | PTE_W)
  mov dword [V2P_WO(entrypml4) + 0*8], eax

  ; entrypml4[PML4X(KERNBASE)] = entrypdpt_high | P|W
  mov eax, V2P_WO(entrypdpt_high)
  or eax, (PTE_P | PTE_W)
  mov dword [V2P_WO(entrypml4) + 511*8], eax

  ; Enable PAE - a long mode prerequisite.
  mov eax, cr4
  or eax, CR4_PAE
  mov cr4, eax

  ; Point CR3 at the PML4.
  mov eax, V2P_WO(entrypml4)
  mov cr3, eax

  ; Set EFER.LME (long mode enable). Must happen before CR0.PG is set
  ; below, or the CPU enters legacy PAE paging instead of long mode.
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
  ; instruction decoding. A far jump's target is always an absolute
  ; segment:offset (never PC-relative), so - unlike the indirect jumps
  ; used below - this can safely use V2P_WO(entry64_high) directly as
  ; the offset.
  lgdt [V2P_WO(entry64gdtdesc)]
  jmp (1<<3):V2P_WO(entry64_high)

BITS 64
entry64_high:
  ; Now genuinely executing 64-bit instructions, but still at a low
  ; physical address (this code is identity-mapped, per entrypd above).
  ; Jump to the high (KERNBASE-relative) virtual address of the code
  ; that follows - the same indirect-jump-through-a-register trick
  ; entry.asm uses to cross from low to high addresses, and for the
  ; same reason: a direct jump assembles as RIP-relative, which would
  ; be wrong here since the jump instruction's *actual* address (low)
  ; differs from the address the linker assumed when computing the
  ; offset (high).
  mov rax, entry64_start
  jmp rax

entry64_start:
  ; Set up a real 64-bit stack pointer and jump to main(), now running
  ; entirely at high virtual addresses.
  mov rsp, (stack + KSTACKSIZE)
  mov rax, main
  jmp rax

section .data
; Page directory/table pages must start on page boundaries, hence the
; alignment. PTE_PS in a page directory entry enables 2MB pages, so
; entrypd is the last level needed - see the comment above entry.
align 4096
global entrypml4
entrypml4:
  times 512 dq 0
align 4096
entrypdpt_low:
  times 512 dq 0
align 4096
entrypdpt_high:
  times 512 dq 0
align 4096
entrypd:
  times 512 dq 0

; Minimal bootstrap GDT used only to reach long mode; vm.c's seginit()
; installs the real per-CPU GDT once main() is running (built in C via
; mmu.h's SEGL()/SEG() macros, not this one).
align 16
entry64gdt:
  dq 0                  ; null descriptor
  SEG64CODE_ASM          ; flat 64-bit code segment, selector 1<<3
entry64gdtdesc:
  dw (entry64gdtdesc - entry64gdt - 1)
  dd V2P_WO(entry64gdt)

section .bss
common stack KSTACKSIZE
