;
; assembler macros to create x86 segments (NASM syntax)
;

%macro SEG_NULLASM 0
        dw 0, 0
        db 0, 0, 0, 0
%endmacro

; The 0xC0 means the limit is in 4096-byte units
; and (for executable segments) 32-bit mode.
; %1 = type, %2 = base, %3 = limit
%macro SEG_ASM 3
        dw (((%3) >> 12) & 0xffff), ((%2) & 0xffff)
        db (((%2) >> 16) & 0xff), (0x90 | (%1)), \
                (0xC0 | (((%3) >> 28) & 0xf)), (((%2) >> 24) & 0xff)
%endmacro

%define STA_X     0x8       ; Executable segment
%define STA_W     0x2       ; Writeable (non-executable segments)
%define STA_R     0x2       ; Readable (executable segments)

; A flat 64-bit code segment: same 8-byte descriptor shape as SEG_ASM
; above, but with L=1 (long mode) instead of D/B=1 - byte-for-byte,
; limit=0xfffff, base=0, G=1, D/B=0, L=1, AVL=0, P=1, DPL=0, S=1,
; type=STA_X|STA_R. Used only by kernel/entry64.asm's tiny bootstrap
; GDT, to reach long mode before kernel/vm.c's seginit() installs the
; real per-CPU GDT (built in C via mmu.h's SEGL() macro instead).
%macro SEG64CODE_ASM 0
        dw 0xffff, 0x0000
        db 0x00, 0x9A, 0xAF, 0x00
%endmacro
