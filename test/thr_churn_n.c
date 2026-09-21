/* thr_churn_n.c — parametrizovatelný thread churn: ROUNDS z argv[1].
 * Slouží k bisectu, od kolika create/join cyklů začne únik/korupce.
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
    for (int i = 0; i < 200; i++) { void *p = malloc(64 + (i % 256)); free(p); }
    if (tls_ctr != (int)id) return (void *)1;
    return NULL;
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    for (int r = 0; r < rounds; r++) {
        pthread_t th[4];
        for (long i = 0; i < 4; i++)
            if (pthread_create(&th[i], NULL, worker, (void *)(r * 4 + i))) return 2;
        for (int i = 0; i < 4; i++) { void *res = NULL; pthread_join(th[i], &res); if (res) return 3; }
    }
    printf("OK thr_churn_n rounds=%d (%d threads)\n", rounds, rounds * 4);
    return 0;
}
