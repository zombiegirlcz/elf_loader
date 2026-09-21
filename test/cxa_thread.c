/* cxa_thread.c — C++ thread_local destruktory (__cxa_thread_atexit_impl).
 *
 * DŮLEŽITÉ: reálné C++ volání předává &__dso_handle, NE NULL.
 *   g++ pro `thread_local` s netriviálním dtor emituje:
 *       __cxa_thread_atexit_impl(dtor, obj, &__dso_handle)
 * Předchozí verze s NULL byla neplatné použití: glibc přeskočí
 * _dl_find_dso_for_object a čte *(TP+0x60) (link_map vlastnícího DSO),
 * což je nula → crash na ldadd [x1=0x4a0].
 *
 * Test proto používá skutečný __dso_handle, stejně jako reálný C++ kód.
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

extern "C" int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
/* __dso_handle je definován v libc/crtstuff a používá ho i g++ pro
 * thread_local dtory. Externí deklarace stačí. */
extern "C" void *__dso_handle;

struct payload { char tag[32]; int n; };

static void payload_dtor(void *p) {
    struct payload *pl = static_cast<struct payload *>(p);
    if (pl->n % 7 == 0) printf("dtor %s n=%d\n", pl->tag, pl->n);
    free(pl);
}

static __thread struct payload *tls_self;

static void *worker(void *arg) {
    long id = (long)arg;
    struct payload *pl = static_cast<struct payload *>(malloc(sizeof *pl));
    snprintf(pl->tag, sizeof pl->tag, "w%ld", id);
    pl->n = (int)(id * 13 + 7);
    tls_self = pl;
    /* Reálné C++ použití: dso_handle = &__dso_handle, ne NULL. */
    __cxa_thread_atexit_impl(payload_dtor, pl, &__dso_handle);
    for (int i = 0; i < 500; i++) {
        void *q = malloc(48 + (i % 300));
        free(q);
    }
    return NULL;
}

int main() {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, (void *)1)) { perror("create"); return 2; }
    pthread_join(t, NULL);
    printf("OK cxa_thread (dso_handle=%p)\n", &__dso_handle);
    return 0;
}