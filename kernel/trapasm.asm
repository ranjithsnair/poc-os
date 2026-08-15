; Trap entry/return (64-bit build). See kernel/trapasm.asm (32-bit
; build) for the overall structure - this is the same alltraps/trapret
; pair, adapted for long mode:
;   - no pusha (doesn't exist in 64-bit mode), so registers are pushed
;     individually, in the order struct trapframe (x86.h) expects
;   - ds/es/fs/gs aren't saved/reloaded at all - segmentation is flat in
;     long mode, so there's nothing meaningful to save or restore (see
;     the struct trapframe comment in x86.h)
;   - trap(tf) is called SysV-style, tf in rdi, not pushed on the stack
;   - iretq instead of iret, popping a 64-bit-wide hardware frame

BITS 64

section .text

extern trap

  ; vectors64.asm sends all traps here.
global alltraps
alltraps:
  ; Build the trap frame: push general-purpose registers, in reverse of
  ; struct trapframe's field order (last pushed = lowest address =
  ; first field).
  push rax
  push rbx
  push rcx
  push rdx
  push rsi
  push rdi
  push rbp
  push r8
  push r9
  push r10
  push r11
  push r12
  push r13
  push r14
  push r15

  ; Call trap(tf), where tf=rsp.
  mov rdi, rsp
  call trap

  ; Return falls through to trapret...
global trapret
trapret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop r11
  pop r10
  pop r9
  pop r8
  pop rbp
  pop rdi
  pop rsi
  pop rdx
  pop rcx
  pop rbx
  pop rax
  add rsp, 16  ; trapno and errcode
  iretq
