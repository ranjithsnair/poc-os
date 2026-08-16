; Initial process execs /init. This code runs in user space. The
; 64-bit syscall ABI is register-based (see kernel/syscall.c's
; argint), so path/argv go in rdi/rsi directly, the same registers a
; normal SysV AMD64 call to exec(path, argv) would use, rather than
; being pushed on the stack.
;
; SYS_execve, not SYS_exec: /usr/bin/init is bash/poc/dinit.c now (a
; real dynamically-linked, PT_INTERP musl binary - poc-os's default
; going forward, see the Makefile's own BASH_PIC_CFLAGS comment), and
; kernel/exec.c's plain exec() has no PT_INTERP/interpreter-loading
; support at all - only execve() does. rdx points at a real (empty)
; envp array, not literal 0: kernel/exec.c's own execve() function
; handles a NULL envp fine (its for-loop checks "envp &&" first), but
; kernel/sysfile.c's sys_execve() syscall wrapper calls fetchargv()
; unconditionally to *fetch* envp out of user memory before execve()
; ever runs - fetchargv(0, ...) tries to read user address 0 and fails,
; so sys_execve returns -1 without ever reaching execve() at all.

#include "syscall.h"
#include "traps.h"

BITS 64
section .text

; execve(init, argv, envp)
global start
start:
  mov rdi, init
  mov rsi, argv
  mov rdx, envp
  mov eax, SYS_execve
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

; char *envp[] = { 0 };
align 8
envp:
  dq 0
