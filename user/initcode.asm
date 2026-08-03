; Initial process execs /init.
; This code runs in user space.

#include "syscall.h"
#include "traps.h"

BITS 32
section .text

; exec(init, argv)
global start
start:
  push argv
  push init
  push 0        ; where caller pc would be
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
align 4
argv:
  dd init
  dd 0
