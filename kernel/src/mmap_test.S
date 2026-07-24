/*
 * Hand-written ring-3 test program exercising SYS_ANON_ALLOCATE/
 * SYS_ANON_FREE now that the latter is a real munmap (not the old
 * no-op): allocates and frees a 4MiB region 60 times in a loop, writing
 * to the first and last byte of each allocation to prove it's really
 * mapped+writable. 60 * 4MiB = 240MiB, comfortably more than this VM's
 * ~222MiB of physical RAM (see the boot log's "physical memory: ... MiB
 * free" line) -- if SYS_ANON_FREE were still a no-op (or otherwise
 * leaking frames), pmm_alloc_frame() would run out and some later
 * iteration's SYS_ANON_ALLOCATE would return 0, since each iteration
 * never reuses the *virtual* address range either (process_anon_allocate
 * is still a bump allocator there -- see its doc comment); succeeding
 * every time is only possible if the *physical* frames genuinely come
 * back each time. A final extra SYS_ANON_FREE on the same (already-freed)
 * base is expected to fail (-1), proving the bookkeeping actually
 * cleared rather than silently succeeding on anything.
 */
.section .text

.global mmap_test_program
.global mmap_test_program_end

mmap_test_program:
    xor %r12, %r12

loop:
    mov $0x400000, %rdi        /* 4 MiB */
    mov $8, %rax                 /* SYS_ANON_ALLOCATE */
    int $0x80
    cmp $0, %rax
    je alloc_fail
    mov %rax, %r13

    movb $0x41, (%r13)
    mov %r13, %r14
    add $0x3FFFFF, %r14
    movb $0x42, (%r14)

    mov %r13, %rdi
    mov $0x400000, %rsi
    mov $9, %rax                    /* SYS_ANON_FREE */
    int $0x80
    cmp $0, %rax
    jne free_fail

    inc %r12
    cmp $60, %r12
    jl loop

    /* One more free of the same (already-freed) base should now fail. */
    mov %r13, %rdi
    mov $0x400000, %rsi
    mov $9, %rax
    int $0x80
    cmp $-1, %rax
    jne double_free_should_have_failed

    mov $0x4D, %rdi           /* 'M' -- everything checked out */
    jmp report

alloc_fail:
    mov $0x41, %rdi              /* 'A' */
    jmp report
free_fail:
    mov $0x46, %rdi               /* 'F' */
    jmp report
double_free_should_have_failed:
    mov $0x44, %rdi                 /* 'D' */
report:
    mov $0, %rax
    int $0x80

    mov $1, %rax                      /* SYS_EXIT */
    mov $0, %rdi
    int $0x80
1:
    jmp 1b
mmap_test_program_end:

.section .note.GNU-stack,"",@progbits
