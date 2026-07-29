#include "syscall.h"
#include "traps.h"

BITS 32
section .text

; The SYS_xxx constant is passed in as its own macro argument (rather than
; built with NASM's %+ paste from the name) so that the C preprocessor -
; which expands SYS_xxx to its numeric value before NASM ever runs - still
; sees the literal token and can resolve it.
;
; "$" before the label forces NASM to treat the name as a plain identifier
; instead of a reserved instruction mnemonic (this matters for "wait",
; which NASM would otherwise parse as the x87 WAIT instruction). It has
; no effect on the symbol name actually emitted.
%macro SYSCALL 2
  global $%1
  $%1:
    mov eax, %2
    int T_SYSCALL
    ret
%endmacro

SYSCALL fork, SYS_fork
SYSCALL exit, SYS_exit
SYSCALL wait, SYS_wait
SYSCALL pipe, SYS_pipe
SYSCALL read, SYS_read
SYSCALL write, SYS_write
SYSCALL close, SYS_close
SYSCALL kill, SYS_kill
SYSCALL exec, SYS_exec
SYSCALL open, SYS_open
SYSCALL mknod, SYS_mknod
SYSCALL unlink, SYS_unlink
SYSCALL fstat, SYS_fstat
SYSCALL link, SYS_link
SYSCALL mkdir, SYS_mkdir
SYSCALL chdir, SYS_chdir
SYSCALL dup, SYS_dup
SYSCALL getpid, SYS_getpid
SYSCALL sbrk, SYS_sbrk
SYSCALL sleep, SYS_sleep
SYSCALL uptime, SYS_uptime
