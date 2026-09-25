/* Diagnosticky helper: loguje free() volane z daneho rozsahu adres volajiciho.
 * LXFREE_LO / LXFREE_HI = hex rozsah return adresy (napr. telo funkce).
 * Pro kazde volani vypise ptr, hlavicku chunku (prev_size, size) a flagy. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void (*real_free)(void *);
static uintptr_t lo, hi;
static int inited;

static uintptr_t watch;
static void watch_hit(const char *what, void *p) {
    if (!watch || (uintptr_t)p != watch) return;
    char b[120];
    int k = snprintf(b, sizeof b, "[lxwatch] %s(%p) backtrace:\n", what, p);
    write(2, b, k);
    /* retez frame pointeru (glibc backtrace() pod loaderem nefunguje) */
    uintptr_t *fp = (uintptr_t *)__builtin_frame_address(0);
    for (int i = 0; i < 24 && fp && !((uintptr_t)fp & 7); i++) {
        uintptr_t lr = fp[1];
        if (!lr) break;
        k = snprintf(b, sizeof b, "[lxwatch]   #%d %#lx\n", i, (unsigned long)lr);
        write(2, b, k);
        uintptr_t *nx = (uintptr_t *)fp[0];
        if (nx <= fp) break;
        fp = nx;
    }
}
static void init(void) {
    inited = 1;
    real_free = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    const char *a = getenv("LXFREE_LO"), *b = getenv("LXFREE_HI");
    if (a) lo = strtoull(a, NULL, 16);
    if (b) hi = strtoull(b, NULL, 16);
    const char *w = getenv("LXFREE_WATCH");
    if (w) watch = strtoull(w, NULL, 16);
}

void free(void *p) {
    if (!inited) init();
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    if (p && lo && ra >= lo && ra < hi) {
        uintptr_t *h = (uintptr_t *)p - 2;
        char b[200];
        int n = snprintf(b, sizeof b,
                         "[lxfree] ra=%#lx ptr=%p prev_size=%#lx size=%#lx flags=%c%c%c\n",
                         (unsigned long)ra, p, (unsigned long)h[0],
                         (unsigned long)(h[1] & ~7UL),
                         (h[1] & 1) ? 'P' : '-', (h[1] & 2) ? 'M' : '-', (h[1] & 4) ? 'A' : '-');
        write(2, b, n);
    }
    watch_hit("free", p);
    if (real_free) real_free(p);
}

/* operator delete(void*, size_t) a delete[](void*) z libstdc++: C++ kod
 * (napr. node::SnapshotData::~SnapshotData) uvolnuje pres ne, free pak
 * volaji jako tail call, takze return adresa ve free() uz neni volajici. */
static void log_del(const char *what, uintptr_t ra, void *p, long sz) {
    if (!inited) init();
    if (!p || !lo || ra < lo || ra >= hi) return;
    uintptr_t *h = (uintptr_t *)p - 2;
    char b[240];
    int n = snprintf(b, sizeof b,
                     "[lxfree] %s ra=%#lx ptr=%p req=%ld prev_size=%#lx size=%#lx flags=%c%c%c\n",
                     what, (unsigned long)ra, p, sz, (unsigned long)h[0],
                     (unsigned long)(h[1] & ~7UL),
                     (h[1] & 1) ? 'P' : '-', (h[1] & 2) ? 'M' : '-', (h[1] & 4) ? 'A' : '-');
    write(2, b, n);
}
/* Vstup v assembleru: x19 volajiciho (adresa prvku + 0x40 ve smycce
 * ~SnapshotData) ulozime DRIV, nez ho prolog C funkce prepise. */
__attribute__((visibility("hidden"))) uintptr_t lx_caller_x19, lx_caller_x20;
__attribute__((visibility("hidden"))) void lx_del_c(void *p, size_t n);
__asm__(".globl _ZdlPvm\n.type _ZdlPvm,%function\n_ZdlPvm:\n"
        "  adrp x16, lx_caller_x19\n"
        "  str x19, [x16, :lo12:lx_caller_x19]\n"
        "  adrp x16, lx_caller_x20\n"
        "  str x20, [x16, :lo12:lx_caller_x20]\n"
        "  b lx_del_c\n.size _ZdlPvm, .-_ZdlPvm\n");
void lx_del_c(void *p, size_t n) {
    static void (*real)(void *, size_t);
    if (!inited) init();
    uintptr_t ra0 = (uintptr_t)__builtin_return_address(0);
    if (p && lo && ra0 >= lo && ra0 < hi && getenv("LXFREE_ELEM")) {
        uintptr_t *e = (uintptr_t *)(lx_caller_x19 - 0x40);
        char b[300];
        int k = snprintf(b, sizeof b, "[lxfree]   elem=%p: %#lx %#lx %#lx %#lx | %#lx %#lx %#lx %#lx\n",
                         (void *)e, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
        write(2, b, k);
    }
    static int dumped;
    if (!dumped && p && lo && ra0 >= lo && ra0 < hi && getenv("LXFREE_THIS")) {
        dumped = 1;
        uintptr_t *t = (uintptr_t *)lx_caller_x20;
        char b[160];
        int k = snprintf(b, sizeof b, "[lxthis] this=%p\n", (void *)t);
        write(2, b, k);
        for (int i = 0; i < 64; i++) {
            k = snprintf(b, sizeof b, "[lxthis] +%3d %#lx\n", i * 8, t[i]);
            write(2, b, k);
        }
    }
    if (!real) real = (void (*)(void *, size_t))dlsym(RTLD_NEXT, "_ZdlPvm");
    watch_hit("delete", p);
    log_del("delete", ra0, p, (long)n);
    real(p, n);
}
void _ZdaPv(void *p) {
    static void (*real)(void *);
    if (!real) real = (void (*)(void *))dlsym(RTLD_NEXT, "_ZdaPv");
    watch_hit("delete[]", p);
    log_del("delete[]", (uintptr_t)__builtin_return_address(0), p, -1);
    real(p);
}

void *realloc(void *p, size_t n) {
    static void *(*real)(void *, size_t);
    if (!real) real = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    if (!inited) init();
    watch_hit("realloc", p);
    return real(p, n);
}

/* __cxa_atexit: loguje registrace funkce LXATEXIT_FN (hex) s backtracem. */
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
    static int (*real)(void (*)(void *), void *, void *);
    if (!real) real = (int (*)(void (*)(void *), void *, void *))dlsym(RTLD_NEXT, "__cxa_atexit");
    const char *w = getenv("LXATEXIT_FN");
    if (w && (uintptr_t)fn == strtoull(w, NULL, 16)) {
        char b[160];
        int k = snprintf(b, sizeof b, "[lxatexit] fn=%p arg=%p dso=%p\n", (void *)fn, arg, dso);
        write(2, b, k);
        uintptr_t *fp = (uintptr_t *)__builtin_frame_address(0);
        for (int i = 0; i < 16 && fp && !((uintptr_t)fp & 7); i++) {
            if (!fp[1]) break;
            k = snprintf(b, sizeof b, "[lxatexit]   #%d %#lx\n", i, (unsigned long)fp[1]);
            write(2, b, k);
            uintptr_t *nx = (uintptr_t *)fp[0];
            if (nx <= fp) break;
            fp = nx;
        }
    }
    return real(fn, arg, dso);
}
