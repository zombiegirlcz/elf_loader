/* thr_churn.c — thread churn: mnohokrát pthread_create + pthread_join.
 * Node/libuv vytváří a ruší vlákna průběžně (worker pool, V8 platform).
 * Každé zrušené vlákno projde _dl_deallocate_tls + free stacku.
 * Když je dealloc/alloc TLS rozbitý, projeví se to jako "double free"
 * až po mnoha iteracích (přesně jako node teardown).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static __thread char tls_buf[512];
static __thread int tls_ctr;

static void *worker(void *arg) {
    long id = (long)arg;
    tls_ctr = (int)id;
    snprintf(tls_buf, sizeof tls_buf, "churn-%ld", id);
    for (int i = 0; i < 200; i++) {
        void *p = malloc(64 + (i % 256));
        free(p);
    }
    if (tls_ctr != (int)id) return (void *)1;
    return NULL;
}

int main(void) {
    enum { ROUNDS = 200, CONC = 4 };
    for (int r = 0; r < ROUNDS; r++) {
        pthread_t th[CONC];
        for (long i = 0; i < CONC; i++) {
            if (pthread_create(&th[i], NULL, worker, (void *)(r * CONC + i))) {
                fprintf(stderr, "FAIL create r=%d i=%ld\n", r, i);
                return 2;
            }
        }
        for (int i = 0; i < CONC; i++) {
            void *res = NULL;
            pthread_join(th[i], &res);
            if (res) { fprintf(stderr, "FAIL join r=%d i=%d\n", r, i); return 3; }
        }
    }
    printf("OK thr_churn (%d threads total)\n", ROUNDS * CONC);
    return 0;
}