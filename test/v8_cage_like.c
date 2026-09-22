/* v8_cage_like.c — napodobuje V8 sandbox: obri PROT_NONE mmap rezervace
 * (radove GB) PRED normalni pthread+malloc praci, pak normalni exit(). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>

static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 3000; i++) {
        size_t sz = 8 + (size_t)((i * 13 + id * 7) % 2048);
        void *p = malloc(sz);
        if (!p) { fprintf(stderr, "FAIL malloc tid=%ld i=%d\n", id, i); return (void *)1; }
        memset(p, (int)i, sz);
        free(p);
    }
    return NULL;
}

int main(void) {
    size_t cage_size = (size_t)4 * 1024 * 1024 * 1024; /* 4GB, jako V8 sandbox */
    void *cage = mmap(NULL, cage_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (cage == MAP_FAILED) { perror("mmap cage"); return 10; }
    printf("cage reserved at %p size=%zu\n", cage, cage_size);

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
    printf("OK v8_cage_like\n");
    return 0;
}
