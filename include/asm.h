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
