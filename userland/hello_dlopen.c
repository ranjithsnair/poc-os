#include <dlfcn.h>
#include <stdio.h>

int main(void) {
    void *handle = dlopen("dlplugin.so", RTLD_NOW);
    if (!handle) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }

    int (*plugin_answer)(void) = dlsym(handle, "plugin_answer");
    if (!plugin_answer) {
        printf("dlsym failed: %s\n", dlerror());
        return 1;
    }

    printf("plugin_answer() = %d\n", plugin_answer());
    dlclose(handle);
    return 0;
}
