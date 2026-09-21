/* thr_heavy.c — těžší scénář blížící se node/V8 startu:
 *  - mnoho vláken (16) současně
 *  - velké __thread bloky (napodobení V8 thread-local)
 *  - pthread_key_create/getspecific/setspecific (libuv style)
 *  - dlopen/dlclose v threadu (node načítá .so lazy)
 *  - pthread_once, mutex, cond
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>

static __thread char big_tls[4096];
static pthread_key_t key;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready = 0;

static void key_dtor(void *p) { free(p); }
static void once_fn(void) { pthread_key_create(&key, key_dtor); }

static void *worker(void *arg) {
    long id = (long)arg;
    snprintf(big_tls, sizeof big_tls, "tls-%ld", id);
    pthread_once(&once, once_fn);
    void *p = malloc(128 + id);
    pthread_setspecific(key, p);
    if (pthread_getspecific(key) != p) { fprintf(stderr, "FAIL key tid=%ld\n", id); return (void *)1; }
    pthread_mutex_lock(&mtx);
    ready++;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mtx);
    void *h = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
    if (h) dlclose(h);
    for (int i = 0; i < 3000; i++) { void *q = malloc(32 + (i % 700)); memset(q, i, 32 + (i % 700)); free(q); }
    printf("worker %ld ok %s\n", id, big_tls);
    return NULL;
}

int main(void) {
    enum { N = 16 };
    pthread_t th[N];
    for (long i = 0; i < N; i++) {
        int rc = pthread_create(&th[i], NULL, worker, (void *)i);
        if (rc) { fprintf(stderr, "FAIL create %ld rc=%d\n", i, rc); return 2; }
    }
    for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
    printf("OK thr_heavy ready=%d\n", ready);
    return 0;
}