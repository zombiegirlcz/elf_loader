/* thr_malloc.c — ještě menší: jen pthread + malloc/free, bez __thread.
 * Slouží k izolaci, zda "double free" pochází z per-thread malloc cache
 * (glibc tcache) přes pthread, nebo z TLS.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 5000; i++) {
        size_t sz = 8 + (size_t)((i * 13 + id * 7) % 2048);
        void *p = malloc(sz);
        if (!p) { fprintf(stderr, "FAIL malloc tid=%ld i=%d\n", id, i); return (void *)1; }
        memset(p, (int)i, sz);
        free(p);
    }
    return NULL;
}

int main(void) {
    enum { N = 4 };
    pthread_t th[N];
    for (long i = 0; i < N; i++) {
        int rc = pthread_create(&th[i], NULL, worker, (void *)i);
        if (rc) { fprintf(stderr, "FAIL pthread_create rc=%d\n", rc); return 2; }
    }
    for (int i = 0; i < N; i++) {
        void *r = NULL;
        pthread_join(th[i], &r);
        if (r) return 3;
    }
    printf("OK thr_malloc\n");
    return 0;
}