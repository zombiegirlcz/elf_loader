/* Testovaci guest helper pro ELF_LOADER_HELPER.
 * Build (v Parrotu, glibc):  gcc -shared -fPIC -O2 -o lxhelper_test.so lxhelper_test.c -ldl
 * Overuje: (1) konstruktor helperu bezi, (2) helper prebije libc symbol
 * volany pres PLT (uname), (3) dlsym(RTLD_NEXT) najde realnou funkci,
 * (4) glibc stdio/malloc/getenv funguji primo pod guest TP. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

__attribute__((constructor)) static void lxhelper_ctor(void) {
    if (getenv("LXHELPER_VERBOSE")) {
        char *m = strdup("ctor");               /* glibc malloc pod guest TP */
        fprintf(stderr, "[lxhelper] %s pid=%d\n", m, (int)getpid());
        free(m);
    }
}

int uname(struct utsname *u) {
    static int (*real)(struct utsname *);
    if (!real)
        real = (int (*)(struct utsname *))dlsym(RTLD_NEXT, "uname");
    if (!real) {
        fprintf(stderr, "[lxhelper] dlsym(RTLD_NEXT, uname) = NULL\n");
        return -1;
    }
    int r = real(u);
    if (r == 0) {
        size_t l = strlen(u->release);
        snprintf(u->release + l, sizeof u->release - l, "+lxhelper");
    }
    return r;
}
