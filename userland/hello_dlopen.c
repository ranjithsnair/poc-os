/* Installed on disk as /hellodl (see main.c): proves dlopen()/dlsym()
 * work end to end by loading dlplugin.c's compiled dlplugin.so at
 * runtime and calling into it, rather than linking against it up front
 * the way hello_libc.c links against libc itself. */
#include <dlfcn.h>
#include <stdio.h>

int main(void) {
    /* Loads /lib/dlplugin.so into this process at runtime. RTLD_NOW means
     * resolve all of its symbols immediately, rather than lazily on first
     * use. */
    void *handle = dlopen("dlplugin.so", RTLD_NOW);
    if (!handle) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }

    /* Looks up the plugin's plugin_answer() function by name and calls it
     * through a function pointer -- this program never saw that function
     * at compile/link time. */
    int (*plugin_answer)(void) = dlsym(handle, "plugin_answer");
    if (!plugin_answer) {
        printf("dlsym failed: %s\n", dlerror());
        return 1;
    }

    printf("plugin_answer() = %d\n", plugin_answer());
    dlclose(handle);
    return 0;
}
