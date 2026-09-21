/* thr_dlopen.c — dlopen/dlclose .so s TLS ve více vláknech.
 * Node načítá nativní moduly a jejich TLS musí být správně přidáno
 * do DTV každého běžícího vlákna (elf_tls_add_module_to_thread).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>

static __thread int tls_self;

static void *worker(void *arg) {
    long id = (long)arg;
    tls_self = (int)id;
    for (int i = 0; i < 50; i++) {
        void *h = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!h) { fprintf(stderr, "FAIL dlopen tid=%ld: %s\n", id, dlerror()); return (void *)1; }
        void *sym = dlsym(h, "sqrt");
        if (!sym) { fprintf(stderr, "FAIL dlsym tid=%ld\n", id); return (void *)1; }
        dlclose(h);
    }
    if (tls_self != (int)id) return (void *)1;
    return NULL;
}

int main(void) {
    enum { N = 6 };
    pthread_t th[N];
    for (long i = 0; i < N; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i)) { perror("create"); return 2; }
    for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
    printf("OK thr_dlopen\n");
    return 0;
}