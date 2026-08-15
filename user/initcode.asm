; Initial process execs /init. This code runs in user space (64-bit
; build). See initcode.asm (32-bit build) for the overall structure -
; the only real difference is how exec's arguments are passed: the
; 64-bit syscall ABI is register-based (see kernel/syscall.c's argint),
; so path/argv go in rdi/rsi directly, the same registers a normal SysV
; AMD64 call to exec(path, argv) would use, rather than being pushed on
; the stack.

#include "syscall.h"
#include "traps.h"

BITS 64
section .text

; exec(init, argv)
global start
start:
  mov rdi, init
  mov rsi, argv
  mov eax, SYS_exec
  int T_SYSCALL

; for(;;) exit();
exit:
  mov eax, SYS_exit
  int T_SYSCALL
  jmp exit

; char init[] = "/usr/bin/init\0";
init:
  db "/usr/bin/init", 0, 0

; char *argv[] = { init, 0 };
align 8
argv:
  dq init
  dq 0
