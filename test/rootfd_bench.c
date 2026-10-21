/* rootfd_bench.c — kapitola 4.3: ma smysl drzet O_PATH fd na $ROOTFS a volat
 * *at(rootfd, "usr/lib/...") misto skladani plne hostove cesty?
 *
 * Meri CISTOU cenu jadra: volame newfstatat/openat primo pres syscall(), takze
 * se netrefime do zadneho shimu loaderu (shimujeme fstatat/openat, ne syscall).
 * A = AT_FDCWD + plna hostova cesta (8 komponent prefixu),
 * B = rootfd + guest-relativni cesta.
 *
 * Build (guest gcc):  gcc -O2 -o /tmp/rootfd_bench /tmp/rootfd_bench.c
 * Run (pod loaderem): elf_loader --ownall $R/tmp/rootfd_bench $R <rel-cesta>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/stat.h>

#define N 20000

static long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <rootfs> <rel-path>\n", argv[0]); return 2; }
    const char *root = argv[1], *rel = argv[2];
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", root, rel);

    int rootfd = (int)syscall(SYS_openat, AT_FDCWD, root, O_PATH | O_DIRECTORY, 0);
    if (rootfd < 0) { perror("openat rootfs O_PATH"); return 2; }

    struct stat st;
    if (syscall(SYS_newfstatat, AT_FDCWD, full, &st, 0) != 0) { perror(full); return 2; }
    if (syscall(SYS_newfstatat, rootfd, rel, &st, 0) != 0) { perror(rel); return 2; }

    printf("full=%s (%zu B)\nrel =%s (%zu B)\n", full, strlen(full), rel, strlen(rel));

    long best_a = 0, best_b = 0;
    for (int round = 0; round < 5; round++) {
        long t0 = now_ns();
        for (int i = 0; i < N; i++) syscall(SYS_newfstatat, AT_FDCWD, full, &st, 0);
        long ta = now_ns() - t0;
        t0 = now_ns();
        for (int i = 0; i < N; i++) syscall(SYS_newfstatat, rootfd, rel, &st, 0);
        long tb = now_ns() - t0;
        if (!best_a || ta < best_a) best_a = ta;
        if (!best_b || tb < best_b) best_b = tb;
        printf("round %d: A(full)=%ld ns/stat  B(rootfd+rel)=%ld ns/stat\n",
               round, ta / N, tb / N);
    }
    printf("\nBEST: A=%ld ns  B=%ld ns  rozdil=%ld ns (%.1f %%)\n",
           best_a / N, best_b / N, (best_a - best_b) / N,
           100.0 * (double)(best_a - best_b) / (double)best_a);
    close(rootfd);
    return 0;
}
