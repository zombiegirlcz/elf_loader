/* v8_like.c — V8/Node-like teardown: mnoho threadů, každý si přes
 * __cxa_thread_atexit_impl zaregistruje destruktor s PLATNÝM &__dso_handle,
 * pak proces skončí přes exit(0) (ne _exit), aby se spustily atexit + dtory.
 *
 * Toto je platné použití (na rozdíl od předchozí verze s NULL).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

extern "C" int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern "C" void *__dso_handle;

struct payload { char tag[32]; int n; };

static void payload_dtor(void *p) {
    struct payload *pl = static_cast<struct payload *>(p);
    if (pl->n % 97 == 0) printf("dtor %s n=%d\n", pl->tag, pl->n);
    free(pl);
}

static __thread struct payload *tls_self;

static void *worker(void *arg) {
    long id = (long)arg;
    struct payload *pl = static_cast<struct payload *>(malloc(sizeof *pl));
    snprintf(pl->tag, sizeof pl->tag, "w%ld", id);
    pl->n = (int)(id * 13 + 7);
    tls_self = pl;
    __cxa_thread_atexit_impl(payload_dtor, pl, &__dso_handle);
    for (int i = 0; i < 2000; i++) {
        void *q = malloc(48 + (i % 900));
        memset(q, i, 48 + (i % 900));
        free(q);
    }
    return NULL;
}

int main() {
    enum { N = 8 };
    pthread_t th[N];
    for (long i = 0; i < N; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i)) { perror("create"); return 2; }
    for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
    printf("v8_like: joining done, calling exit(0)\n");
    fflush(stdout);
    exit(0);
}