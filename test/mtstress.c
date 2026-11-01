/* mtstress.c — minimalni repro pro obcasnou heap corruption pod loaderem.
 *
 * Vzor: git pack-objects s 8 vlakny pada v __libc_free (pointer = smeti).
 * Tady: N vlaken, kazde hodne malloc/free/realloc ruznych velikosti (aby se
 * zapojily per-thread areny), plus mmap/munmap. Kazda alokace ma hlavicku
 * s magic + kontrolnim bajtem; free() ji overi PRED uvolnenim, takze se
 * corruption pozna hned a nerekne jen "smeti ve free".
 *
 * Build (guest gcc pod loaderem):
 *   gcc -O1 -pthread -o mtstress mtstress.c
 * Run:
 *   ./mtstress <threads> <cycles>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAGIC 0x4d545354u          /* "MTST" */
#define MAXSZ 4096

typedef struct {
    unsigned magic;
    unsigned idx;
    size_t   sz;
} hdr_t;

static volatile int g_fail;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void report(const char *what, unsigned long v)
{
    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "FAIL %s: 0x%lx\n", what, v);
    g_fail = 1;
    pthread_mutex_unlock(&g_lock);
}

static void *worker(void *arg)
{
    unsigned tid = (unsigned)(unsigned long)arg;
    unsigned seed = tid * 2654435761u + 12345u;
    void *slots[64];
    memset(slots, 0, sizeof slots);

    for (unsigned c = 0; c < 20000; c++) {
        seed = seed * 1103515245u + 12345u;
        unsigned k = (seed >> 8) % 64;
        unsigned op = (seed >> 20) % 3;

        if (slots[k] && op == 0) {
            hdr_t *h = (hdr_t *)slots[k] - 1;
            if (h->magic != MAGIC) report("magic-on-free", h->magic);
            else if (h->idx != k)  report("idx-on-free", h->idx);
            else {
                unsigned char *p = (unsigned char *)slots[k];
                unsigned char want = (unsigned char)(h->idx * 7u + 1u);
                for (size_t i = 0; i < h->sz; i += 61)
                    if (p[i] != want) { report("payload", i); break; }
            }
            free(h);
            slots[k] = NULL;
        } else if (slots[k] && op == 1) {
            hdr_t *h = (hdr_t *)slots[k] - 1;
            size_t nsz = (seed >> 4) % MAXSZ + 1;
            hdr_t *nh = realloc(h, sizeof(hdr_t) + nsz);
            if (!nh) { report("realloc-null", c); continue; }
            if (nh->magic != MAGIC) report("magic-on-realloc", nh->magic);
            nh->idx = k;
            nh->sz = nsz;
            /* po zvetseni jsou nove bajty nedefinovane -> zapsat cely vzor
             * znovu, jinak by kontrola hlasila falesny FAIL. */
            unsigned char *np = (unsigned char *)(nh + 1);
            unsigned char nw = (unsigned char)(k * 7u + 1u);
            for (size_t i = 0; i < nsz; i += 61) np[i] = nw;
            slots[k] = np;
        } else {
            size_t sz = (seed >> 4) % MAXSZ + 1;
            hdr_t *h = malloc(sizeof(hdr_t) + sz);
            if (!h) { report("malloc-null", c); continue; }
            h->magic = MAGIC;
            h->idx = k;
            h->sz = sz;
            unsigned char *p = (unsigned char *)(h + 1);
            unsigned char want = (unsigned char)(k * 7u + 1u);
            for (size_t i = 0; i < sz; i += 61) p[i] = want;
            slots[k] = p;
        }

        if ((c & 127) == 0) {
            size_t ml = 4096 * (((seed >> 12) % 16) + 1);
            void *m = mmap(NULL, ml, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m != MAP_FAILED) {
                memset(m, (int)tid, ml);
                munmap(m, ml);
            }
        }
    }
    for (unsigned k = 0; k < 64; k++)
        if (slots[k]) free((hdr_t *)slots[k] - 1);
    return NULL;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 8;
    if (n < 1) n = 1;
    if (n > 256) n = 256;
    fprintf(stderr, "mtstress threads=%d\n", n);

    pthread_t th[256];
    for (int i = 0; i < n; i++) {
        if (pthread_create(&th[i], NULL, worker, (void *)(unsigned long)i) != 0) {
            fprintf(stderr, "pthread_create failed at %d\n", i);
            return 2;
        }
    }
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);

    if (g_fail) { fprintf(stderr, "RESULT: FAIL\n"); return 1; }
    fprintf(stderr, "RESULT: OK\n");
    return 0;
}