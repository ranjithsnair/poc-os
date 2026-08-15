; Stage 2's real entry point - the only thing guaranteed to land at
; STAGE2_ADDR (boot/bootmain.c) once boot2main.c's C code is objcopy'd
; out to a raw binary. "-e start" on its own (see the Makefile) only
; sets ELF entry-point *metadata* - meaningless once the linked ELF is
; flattened to a raw blob and jumped to by hardcoded address, since
; whichever function the compiler happened to place first in .text is
; what's actually sitting at that address, not necessarily start().
; Passing this object file first to the linker (matching bootasm.asm's
; own role ahead of bootmain.o for the exact same reason) guarantees
; *this* tiny stub is what's first instead, and its call to start() is
; an ordinary linker-resolved symbol reference - correct regardless of
; however boot2main.c's own functions get ordered.

BITS 32

extern start
global entry2
entry2:
  call start
spin2:
  jmp spin2
