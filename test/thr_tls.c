/* thr_tls.c — minimální glibc test pro reprodukci "double free" a TLS
 * chování child threadů pod elf_loaderem (node vs python).
 *
 * 1) hlavní thread: __thread proměnná, malloc/free smyčka
 * 2) pthread_create N vláken; každé:
 *      - má vlastní __thread hodnotu (ověříme, že není sdílená)
 *      - provede malloc/free smyčku
 *      - přečte __thread přes getter (GD/LD model)
 * 3) pthread_join všech; závěrečný malloc/free v main.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static __thread int tls_val = 100;
static __thread char tls_buf[64];

static int get_tls_val(void) { return tls_val; }

static void *worker(void *arg) {
    long id = (long)arg;
    tls_val = (int)(1000 + id);
    snprintf(tls_buf, sizeof tls_buf, "thread-%ld", id);

    if (tls_val != 1000 + id) {
        fprintf(stderr, "FAIL tid=%ld tls_val=%d\n", id, tls_val);
        return (void *)1;
    }
    if (get_tls_val() != 1000 + id) {
        fprintf(stderr, "FAIL tid=%ld getter=%d\n", id, get_tls_val());
        return (void *)1;
    }
    if (strcmp(tls_buf, "") == 0) {
        fprintf(stderr, "FAIL tid=%ld tls_buf empty\n", id);
        return (void *)1;
    }
    /* malloc/free smyčka — simuluje V8/libuv per-thread cache */
    for (int i = 0; i < 2000; i++) {
        size_t sz = 16 + (size_t)((i * 37 + id * 11) % 4096);
        void *p = malloc(sz);
        if (!p) { fprintf(stderr, "FAIL malloc tid=%ld i=%d\n", id, i); return (void *)1; }
        memset(p, (int)(i & 0xff), sz);
        free(p);
    }
    printf("thread %ld ok tls=%d buf=%s\n", id, tls_val, tls_buf);
    return NULL;
}

int main(void) {
    if (tls_val != 100)
        fprintf(stderr, "WARN main tls_val=%d (expected 100)\n", tls_val);

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
    /* závěrečná smyčka v main */
    for (int i = 0; i < 4000; i++) {
        void *p = malloc(64 + (i % 512));
        if (!p) return 4;
        free(p);
    }
    printf("OK main tls=%d getter=%d\n", tls_val, get_tls_val());
    return 0;
}