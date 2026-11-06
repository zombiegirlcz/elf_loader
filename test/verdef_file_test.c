/* Host unit test pro src/verdef.h — cteni DT_VERDEF ze souboru.
 *
 * Build & run (soucast `make test-host`):
 *   gcc -I./src -o /tmp/vd_test test/verdef_file_test.c && /tmp/vd_test
 *
 * Testovaci .so se vyrobi za behu hostim gcc s version scriptem, aby test
 * nezavisel na zadne konkretni binarce v rootfs. Negativni pripady (jine
 * jmeno verze, ELF bez DT_VERDEF, neexistujici soubor, ne-ELF) chrani proti
 * tomu, aby detekce vracela 1 "vzdycky".
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>

static long shim_raw_syscall6(long nr, long a0, long a1, long a2, long a3,
                              long a4, long a5) {
    return syscall(nr, a0, a1, a2, a3, a4, a5);
}
static int raw_open(const char *path, int flags) {
    if (!path) return -1;
    return (int)syscall(SYS_openat, -100 /* AT_FDCWD */, path, flags, 0);
}
static int raw_close(int fd) { return (int)syscall(SYS_close, fd); }

#include "verdef.h"

static int fails;
static void check(const char *what, int got, int want) {
    if (got == want) {
        printf("OK: %s -> %d\n", what, got);
    } else {
        printf("FAIL: %s -> %d (ocekavano %d)\n", what, got, want);
        fails++;
    }
}

/* Postav .so s danym jmenem verze; vrati 0 pri uspechu. */
static int build_so(const char *ver, const char *out) {
    char cmd[1024];
    FILE *f = fopen("/tmp/vd_src.c", "w");
    if (!f) return -1;
    fputs("int vd_sym(void){return 42;}\n", f);
    fclose(f);
    if (ver) {
        f = fopen("/tmp/vd_map.ver", "w");
        if (!f) return -1;
        fprintf(f, "%s { global: vd_sym; local: *; };\n", ver);
        fclose(f);
        snprintf(cmd, sizeof cmd,
                 "gcc -shared -fPIC -o %s /tmp/vd_src.c "
                 "-Wl,--version-script=/tmp/vd_map.ver 2>/dev/null", out);
    } else {
        snprintf(cmd, sizeof cmd,
                 "gcc -shared -fPIC -o %s /tmp/vd_src.c 2>/dev/null", out);
    }
    return system(cmd) == 0 ? 0 : -1;
}

int main(void) {
    if (build_so("BUN_1.2", "/tmp/vd_bun.so") != 0) {
        printf("SKIP: hosti gcc neumi postavit testovaci .so\n");
        return 0;
    }
    if (build_so("GLIBC_2.17", "/tmp/vd_glibc.so") != 0 ||
        build_so(NULL, "/tmp/vd_plain.so") != 0) {
        printf("SKIP: hosti gcc neumi postavit testovaci .so\n");
        return 0;
    }

    check("BUN_1.2 .so, prefix \"BUN_\"",
          elf_file_has_verdef_prefix("/tmp/vd_bun.so", "BUN_"), 1);
    check("BUN_1.2 .so, prefix \"BUN_1.2\" (cela verze)",
          elf_file_has_verdef_prefix("/tmp/vd_bun.so", "BUN_1.2"), 1);
    check("BUN_1.2 .so, prefix \"GLIBC_\" (jiny prefix)",
          elf_file_has_verdef_prefix("/tmp/vd_bun.so", "GLIBC_"), 0);
    check("GLIBC_2.17 .so, prefix \"BUN_\"",
          elf_file_has_verdef_prefix("/tmp/vd_glibc.so", "BUN_"), 0);
    check("GLIBC_2.17 .so, prefix \"GLIBC_\"",
          elf_file_has_verdef_prefix("/tmp/vd_glibc.so", "GLIBC_"), 1);
    check(".so bez version scriptu (zadny DT_VERDEF)",
          elf_file_has_verdef_prefix("/tmp/vd_plain.so", "BUN_"), 0);
    check("neexistujici soubor",
          elf_file_has_verdef_prefix("/tmp/vd_neexistuje.so", "BUN_"), 0);
    check("ne-ELF soubor (vlastni .c)",
          elf_file_has_verdef_prefix("/tmp/vd_src.c", "BUN_"), 0);
    check("NULL cesta", elf_file_has_verdef_prefix(NULL, "BUN_"), 0);
    check("prazdny prefix",
          elf_file_has_verdef_prefix("/tmp/vd_bun.so", ""), 0);

    if (fails) { printf("%d FAIL\n", fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
