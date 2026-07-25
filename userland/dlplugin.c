/*
 * A minimal shared library with no relation to libc.so/ld.so themselves
 * -- exists purely so hello_dlopen.c has something real to dlopen()/
 * dlsym() at runtime, proving the dynamic linker's runtime loading path
 * (not just the PT_INTERP startup path every other userland/%.dyn.elf
 * program already exercises) works end to end.
 */
int plugin_answer(void) {
    return 42;
}
