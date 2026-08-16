; Context switch
;
;   void swtch(struct context **old, struct context *new);
;
; Save the current registers on the stack, creating a struct context,
; and save its address in *old. Switch stacks to new and pop
; previously-saved registers, using the SysV AMD64 calling convention
; (old in rdi, new in rsi) and its callee-saved register set (rbx, rbp,
; r12-r15).

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
