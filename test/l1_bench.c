/* l1_bench.c — cena L1 stubu na jeden cestovy syscall (prompt 04).
 * Pousti se POD LOADEREM s ELF_LOADER_L1=0/1, rozdil ns/volani = cena stubu.
 *   rel: fstatat(dirfd, "ls")      - jako find/fts: nic se neprekladа
 *   abs: fstatat(AT_FDCWD, "/usr/bin/ls") - guest cesta, preklad
 * Build (guest gcc): gcc -O2 -o l1_bench test/l1_bench.c */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#define N 200000
static long ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000L + t.tv_nsec; }
int main(void) {
    struct stat st;
    int d = open("/usr/bin", O_RDONLY | O_DIRECTORY);
    if (d < 0) { perror("open /usr/bin"); return 1; }
    long best_r = 1L << 62, best_a = 1L << 62;
    for (int k = 0; k < 5; k++) {
        long t0 = ns();
        for (int i = 0; i < N; i++) fstatat(d, "ls", &st, AT_SYMLINK_NOFOLLOW);
        long t1 = ns();
        for (int i = 0; i < N; i++) fstatat(AT_FDCWD, "/usr/bin/ls", &st, AT_SYMLINK_NOFOLLOW);
        long t2 = ns();
        if (t1 - t0 < best_r) best_r = t1 - t0;
        if (t2 - t1 < best_a) best_a = t2 - t1;
    }
    printf("rel=%ld ns abs=%ld ns\n", best_r / N, best_a / N);
    return 0;
}
