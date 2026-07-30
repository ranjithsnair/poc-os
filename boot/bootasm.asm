#include "asm.h"
#include "memlayout.h"
#include "mmu.h"

; Start the first CPU: switch to 32-bit protected mode, jump into C.
; The BIOS loads this code from the first sector of the hard disk into
; memory at physical address 0x7c00 and starts executing in real mode
; with cs=0 ip=7c00.

BITS 16

extern bootmain
global start
start:
  cli                       ; BIOS enabled interrupts; disable

  ; Zero data segment registers DS, ES, and SS.
  xor ax, ax                ; Set ax to zero
  mov ds, ax                ; -> Data Segment
  mov es, ax                ; -> Extra Segment
  mov ss, ax                ; -> Stack Segment

  ; Physical address line A20 is tied to zero so that the first PCs
  ; with 2 MB would run software that assumed 1 MB.  Undo that.
seta20.1:
  in al, 0x64                ; Wait for not busy
  test al, 0x2
  jnz seta20.1

  mov al, 0xd1                ; 0xd1 -> port 0x64
  out 0x64, al

seta20.2:
  in al, 0x64                ; Wait for not busy
  test al, 0x2
  jnz seta20.2

  mov al, 0xdf                ; 0xdf -> port 0x60
  out 0x60, al

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

  ; Set up the stack pointer and call into C.
  mov esp, start
  call bootmain

  ; If bootmain returns (it shouldn't), trigger a Bochs
  ; breakpoint if running under Bochs, then loop.
  mov ax, 0x8a00            ; 0x8a00 -> port 0x8a00
  mov dx, ax
  out dx, ax
  mov ax, 0x8ae0            ; 0x8ae0 -> port 0x8a00
  out dx, ax
spin:
  jmp spin

; Bootstrap GDT
align 4                                   ; force 4 byte alignment
gdt:
  SEG_NULLASM                             ; null seg
  SEG_ASM STA_X|STA_R, 0x0, 0xffffffff    ; code seg
  SEG_ASM STA_W, 0x0, 0xffffffff          ; data seg

gdtdesc:
  dw (gdtdesc - gdt - 1)             ; sizeof(gdt) - 1
  dd gdt                             ; address gdt
