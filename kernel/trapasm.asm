#include "mmu.h"

BITS 32

section .text

extern trap

  ; vectors.asm sends all traps here.
global alltraps
alltraps:
  ; Build trap frame.
  push ds
  push es
  push fs
  push gs
  pushad

  ; Set up data segments.
  mov ax, (SEG_KDATA<<3)
  mov ds, ax
  mov es, ax

  ; Call trap(tf), where tf=esp
  push esp
  call trap
  add esp, 4

  ; Return falls through to trapret...
global trapret
trapret:
  popad
  pop gs
  pop fs
  pop es
  pop ds
  add esp, 0x8  ; trapno and errcode
  iret
