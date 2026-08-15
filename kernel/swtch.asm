; Context switch (64-bit build)
;
;   void swtch(struct context **old, struct context *new);
;
; Save the current registers on the stack, creating a struct context,
; and save its address in *old. Switch stacks to new and pop
; previously-saved registers. See kernel/swtch.asm (32-bit build) for
; the full explanation - this is the same routine, just SysV AMD64
; calling convention (old in rdi, new in rsi, instead of the stack) and
; SysV's wider set of callee-saved registers (rbx, rbp, r12-r15).

BITS 64

section .text
global swtch
swtch:
  ; Save old callee-saved registers
  push rbp
  push rbx
  push r12
  push r13
  push r14
  push r15

  ; Switch stacks
  mov [rdi], rsp
  mov rsp, rsi

  ; Load new callee-saved registers
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  pop rbp
  ret
