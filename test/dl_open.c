/* dl_open.c — izoluje dlopen/dlclose mnoha .so včetně těch s TLS.
 * node dělá přesně tohle při startu (icu, openssl, ...).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

int main(void) {
    const char *libs[] = {
        "libm.so.6", "libdl.so.2", "libpthread.so.0",
        "libz.so.1", "libstdc++.so.6", "libgcc_s.so.1",
        "libcrypto.so.3", "libssl.so.3", "libicuuc.so", "libzstd.so.1",
    };
    for (size_t i = 0; i < sizeof libs / sizeof libs[0]; i++) {
        void *h = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
        if (!h) { printf("dlopen %s -> %s\n", libs[i], dlerror()); continue; }
        printf("dlopen %s OK\n", libs[i]);
        dlclose(h);
    }
    printf("OK dl_open\n");
    return 0;
}