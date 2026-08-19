#include <dlfcn.h>

void *dlopen(const char *filename, int flags) {
    (void)filename;
    (void)flags;
    return NULL;
}

void *dlsym(void *handle, const char *symbol) {
    (void)handle;
    (void)symbol;
    return NULL;
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}

int dladdr(const void *addr, Dl_info *info) {
    (void)addr;
    (void)info;
    return 0;
}
