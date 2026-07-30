; The poc kernel starts executing in this file. This file is linked with
; the kernel C code, so it can refer to kernel symbols such as main().
; The boot block (bootasm.asm and bootmain.c) jumps to entry below.

; Multiboot header, for multiboot boot loaders like GNU Grub.
; http://www.gnu.org/software/grub/manual/multiboot/multiboot.html
;
; Using GRUB 2, you can boot poc from a file stored in a
; Linux file system by copying kernel or kernelmemfs to /boot
; and then adding this menu entry:
;
; menuentry "poc" {
; 	insmod ext2
; 	set root='(hd0,msdos1)'
; 	set kernel='/boot/kernel'
; 	echo "Loading ${kernel}..."
; 	multiboot ${kernel} ${kernel}
; 	boot
; }

#include "asm.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"

BITS 32

extern main
extern entrypgdir

; Multiboot header.  Data to direct multiboot loader.
align 4
section .text
global multiboot_header
multiboot_header:
  %define magic 0x1badb002
  %define flags 0
  dd magic
  dd flags
  dd (-magic-flags)

; By convention, the _start symbol specifies the ELF entry point.
; Since we haven't set up virtual memory yet, our entry point is
; the physical address of 'entry'.
global _start
_start equ V2P_WO(entry)

; Entering poc on boot processor, with paging off.
global entry
entry:
  ; Turn on page size extension for 4Mbyte pages
  mov eax, cr4
  or eax, (CR4_PSE)
  mov cr4, eax
  ; Set page directory
  mov eax, (V2P_WO(entrypgdir))
  mov cr3, eax
  ; Turn on paging.
  mov eax, cr0
  or eax, (CR0_PG|CR0_WP)
  mov cr0, eax

  ; Set up the stack pointer.
  mov esp, (stack + KSTACKSIZE)

  ; Jump to main(), and switch to executing at
  ; high addresses. The indirect jump is needed because
  ; the assembler would otherwise produce a PC-relative
  ; instruction for a direct jump.
  mov eax, main
  jmp eax

common stack KSTACKSIZE
