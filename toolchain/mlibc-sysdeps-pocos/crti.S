/*
 * Provides the *prologue* half of `_init`/`_fini` -- GCC's own
 * crtbegin.o/crtend.o (toolchain/cross/lib/gcc/.../crtbegin.o) only
 * contribute the *middle* of these two functions (a couple of `call`s
 * into frame_dummy/__do_global_ctors_aux), assuming a real libc's crti.o/
 * crtn.o provide the surrounding function prologue/epilogue -- glibc's
 * csu/crti.S is the reference for this exact pattern. Without this,
 * `.init`/`.fini` end up as bare `call`/`call` sequences with no
 * enclosing `ret`, so falling into either one runs off the end into
 * whatever memory happens to follow and crashes (found while bringing
 * up bash: its C++-based mlibc runtime has real global constructors,
 * unlike the trivial C test programs from Phase 1/2, so this was the
 * first program to actually exercise the broken .init/.fini).
 *
 * Link order matters: crt1.o crti.o crtbegin.o <objects> crtend.o
 * crtn.o -- crti.S's and crtn.S's identically-named `.init`/`.fini`
 * sections concatenate with crtbegin.o's/crtend.o's contributions in
 * between, forming one complete function each.
 */
.section .init
.global _init
_init:
    push %rbp
    mov %rsp, %rbp

.section .fini
.global _fini
_fini:
    push %rbp
    mov %rsp, %rbp

.section .note.GNU-stack,"",@progbits
