/* proot_compat_test.c — host unit test for src/proot_compat.h parser half.
 *
 * Testuje jen proot_compat_parse() (bezi pod bionickym/host TP, getenv/
 * setenv jsou tu bezpecne). Druha (TLS-sensitive) cast hlavicky se vubec
 * nevola, ale musi se zkompilovat — stubujeme jeji raw_/shim_ zavislosti
 * minimalne tak, aby se hlavicka includla beze zmeny.
 *
 * Build: gcc -I../src -o /tmp/pc_test proot_compat_test.c && /tmp/pc_test
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* ---- stuby pro TLS-sensitive pulku proot_compat.h (nevolaji se) -------- */
static size_t shim_strlen(const char *s) { return strlen(s); }
static int shim_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static char *shim_strchr(const char *s, int c) { return strchr((char *)s, c); }
static size_t shim_strcpy(char *d, size_t n, const char *s) {
    size_t l = strlen(s);
    if (l + 1 > n) l = n ? n - 1 : 0;
    memcpy(d, s, l);
    d[l] = 0;
    return l;
}
static size_t shim_strcat(char *d, size_t n, const char *s) {
    size_t dl = strlen(d);
    return shim_strcpy(d + dl, n - dl, s);
}
static int raw_access(const char *path, int mode) { (void)path; (void)mode; return -1; }
static int raw_open(const char *path, int flags) { (void)path; (void)flags; return -1; }
static ssize_t raw_read(int fd, void *buf, size_t n) { (void)fd; (void)buf; (void)n; return -1; }
static int raw_close(int fd) { (void)fd; return 0; }
static long shim_raw_syscall6(long nr, long a, long b, long c, long d, long e, long f) {
    (void)nr; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return -1;
}
typedef int (*fp_execve)(const char *, char *const[], char *const[]);
static void *g_orig_execve = NULL;
/* bind_reverse (src/binds.h) - nevola se v testovane casti (parser), ale
 * exec_real pulka hlavicky na ni odkazuje -> musi se zkompilovat. */
static int bind_reverse(const char *host, char *out, size_t n) {
    (void)host; (void)out; (void)n; return 0;
}

#include "../src/proot_compat.h"

static int fails = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

/* presne argv, ktere posila assets/usr/bin/boot */
static void test_boot_flags_are_accepted(void) {
    char *argv[] = { "--kill-on-exit", "--link2symlink", "-L", "--change-id=0:0",
                      "--sysvipc", "-r", "/tmp/rootfs", "-w", "/root", "--", "id" };
    int cmd_start = -1, root_id = -1;
    unsetenv("LX_FAKEROOT"); unsetenv("LX_L2S");
    /* LX_PROOT_SYSVIPC=strict je dokumentovany escape hatch, takze ho muze
     * mit kdokoliv exportovany v shellu; pak parse vrati -1 a cmd_start
     * zustane na -1. CHECK neprerusuje beh, takze bez teto kontroly by
     * nasledoval argv[-1]. */
    unsetenv("LX_PROOT_SYSVIPC");
    int rc = proot_compat_parse(11, argv, &cmd_start, &root_id);
    CHECK(rc == 0);
    CHECK(cmd_start >= 0 && cmd_start < 11 &&
          strcmp(argv[cmd_start], "id") == 0);
    CHECK(root_id == 1);                                  /* --change-id=0:0 -> fakeroot */
    CHECK(getenv("LX_FAKEROOT") && strcmp(getenv("LX_FAKEROOT"), "1") == 0);
    CHECK(getenv("LX_L2S") && strcmp(getenv("LX_L2S"), "1") == 0);  /* --link2symlink */
    CHECK(getenv("ROOTFS") && strcmp(getenv("ROOTFS"), "/tmp/rootfs") == 0);
}

static void test_change_id_nonzero_falls_back(void) {
    char *argv[] = { "-r", "/tmp/rootfs", "--change-id=1000:1000", "--", "id" };
    int cmd_start = -1, root_id = -1;
    CHECK(proot_compat_parse(5, argv, &cmd_start, &root_id) == -1);
}

static void test_unknown_option_still_falls_back(void) {
    char *argv[] = { "-r", "/tmp/rootfs", "-S", "/x", "--", "id" };
    int cmd_start = -1, root_id = -1;
    CHECK(proot_compat_parse(6, argv, &cmd_start, &root_id) == -1);
}

static void test_sysvipc_strict_falls_back(void) {
    char *argv[] = { "-r", "/tmp/rootfs", "--sysvipc", "--", "id" };
    int cmd_start = -1, root_id = -1;
    setenv("LX_PROOT_SYSVIPC", "strict", 1);
    CHECK(proot_compat_parse(5, argv, &cmd_start, &root_id) == -1);
    unsetenv("LX_PROOT_SYSVIPC");
}

int main(void) {
    test_boot_flags_are_accepted();
    test_change_id_nonzero_falls_back();
    test_unknown_option_still_falls_back();
    test_sysvipc_strict_falls_back();
    if (fails) {
        fprintf(stderr, "%d assertion(s) failed\n", fails);
        return 1;
    }
    printf("OK: all proot_compat_parse tests passed\n");
    return 0;
}
