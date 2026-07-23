/*
 * Hand-written ring-3 test program: prints "Hi\n" one character at a
 * time via SYS_WRITE_CHAR (each character passed by value in a
 * register, never through memory) and then calls SYS_EXIT.
 *
 * process.c's process_create() copies the raw bytes between these two
 * symbols into a freshly allocated physical frame and maps that frame
 * executable+user in the new process's own address space, and the
 * scheduler (also process.c) jumps to it in ring3. For that copy to run
 * correctly at an address different from wherever this
 * translation unit happens to get linked in the kernel image, the code
 * must not reference anything by absolute address: every instruction
 * here is either a register/immediate op or the final PC-relative
 * jump-to-self, so the byte sequence means the same thing no matter
 * where it's placed in memory.
 */
.section .text

.global user_test_program
.global user_test_program_end

user_test_program:
    mov $0x48, %rdi     /* 'H' */
    mov $0, %rax        /* SYS_WRITE_CHAR */
    int $0x80
    mov $0x69, %rdi     /* 'i' */
    mov $0, %rax
    int $0x80
    mov $0x0A, %rdi     /* '\n' */
    mov $0, %rax
    int $0x80
    mov $1, %rax        /* SYS_EXIT */
    int $0x80
1:
    jmp 1b              /* unreached: SYS_EXIT halts the CPU for good */
user_test_program_end:

.section .note.GNU-stack,"",@progbits
