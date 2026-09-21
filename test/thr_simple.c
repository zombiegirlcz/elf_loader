/* thr_simple.c — absolutně minimální: 1 thread, který jen vrátí hodnotu.
 * Když spadne i tohle, problém je v pthread_create/allocate_stack/TLS.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>

static __thread int tls_val = 7;

static void *worker(void *arg) {
    (void)arg;
    tls_val = 99;
    printf("worker tls=%d\n", tls_val);
    return NULL;
}

int main(void) {
    pthread_t t;
    int rc = pthread_create(&t, NULL, worker, NULL);
    if (rc) { fprintf(stderr, "FAIL pthread_create rc=%d\n", rc); return 2; }
    pthread_join(t, NULL);
    printf("OK thr_simple main tls=%d\n", tls_val);
    return 0;
}