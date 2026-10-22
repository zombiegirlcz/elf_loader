/* l1_logic_test.c — host test C logiky L1 (src/l1.h): sloty prekladovych
 * bufferu, l1_before/l1_after nad ramcem (bez asm a bez zarizeni).
 *
 * l1.h se v main.c vklada pod GUEST TP a pouziva jeho primitiva; tady je
 * dodavame jako bezne funkce. shim_translate je zjednodusena (prefix +
 * idempotence + excluded) - jeji plnou verzi pokryva test-all.sh.
 *
 * Build: cc -Wall -Iinclude -Isrc test/l1_logic_test.c -o /tmp/l1lt
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static const char *g_shim_root = "/data/R";
static int g_cwd_keep_host = 0;
static char g_guest_exe_path[256] = "/data/R/usr/bin/prog";

static size_t shim_strlen(const char *s) { size_t i = 0; while (s[i]) i++; return i; }
static int shim_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static void shim_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static void shim_strcpy(char *d, size_t n, const char *s) { if (n) { strncpy(d, s, n - 1); d[n - 1] = 0; } }
static void shim_strcat(char *d, size_t n, const char *s) { size_t l = strlen(d); if (l < n) shim_strcpy(d + l, n - l, s); }
static int shim_excluded(const char *p) {
    return !strncmp(p, "/proc", 5) || !strncmp(p, "/system", 7) || !strncmp(p, "/data", 5);
}
static long shim_raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    return syscall(nr, a0, a1, a2, a3, a4, a5);
}
#include "relsym.h"
static int shim_translate(const char *p, char *out, size_t n) {
    if (!p || p[0] != '/') return 0;
    size_t rl = strlen(g_shim_root);
    if (!strncmp(p, g_shim_root, rl) && (p[rl] == '/' || !p[rl])) return 0;
    if (shim_excluded(p)) return 0;
    if (rl + strlen(p) + 1 > n) return 0;
    snprintf(out, n, "%s%s", g_shim_root, p);
    return 1;
}
static int shim_is_proc_self_exe(const char *p) { return p && !strcmp(p, "/proc/self/exe"); }
/* binds (prompt 06), zjednodusene: ro pod /ro/, mountpoint /mnt/b, jehoz
 * host cil je /data/R/srv/b. Plne verze pokryva test-all.sh binds. */
static int shim_sys_intercept(long nr, const unsigned long *a, long *res) {
    const char *p = (const char *)a[1];
    if ((nr == 34 || nr == 35) && (long)a[0] == -100 && p && !strncmp(p, "/ro/", 4)) { *res = -30; return 1; }
    if (nr == 35 && (long)a[0] == -100 && p && !strcmp(p, "/mnt/b")) { *res = -16; return 1; }
    if (nr == 56 && p && !strcmp(p, "/proc/self/cmdline")) { *res = 0; return 1; }   /* fd 0 je platny vysledek */
    return 0;
}
static size_t bind_reverse_inplace(char *buf, size_t cap) {
    const char *h = "/data/R/srv/b", *g = "/mnt/b";
    size_t hl = strlen(h), gl = strlen(g);
    if (strncmp(buf, h, hl) || (buf[hl] && buf[hl] != '/')) return 0;
    size_t rest = strlen(buf + hl);
    if (gl + rest + 1 > cap) return 0;
    memmove(buf + gl, buf + hl, rest + 1);
    memcpy(buf, g, gl);
    return gl + rest;
}
#include "l1.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else printf("ok: %s\n", msg); \
} while (0)

/* ramec jako v l1_common: frame[0..7] puvodni, [8] nr, [20] vysledek,
 * [21] slot, [22..29] argumenty syscallu */
static void mkframe(unsigned long *f, long nr, unsigned long a0, unsigned long a1,
                    unsigned long a2, unsigned long a3) {
    memset(f, 0, 94 * sizeof *f);
    f[0] = a0; f[1] = a1; f[2] = a2; f[3] = a3; f[8] = (unsigned long)nr;
    for (int i = 0; i < 8; i++) f[22 + i] = f[i];
}

int main(void) {
    CHECK(l1_slots_init(), "sloty namapovane");
    unsigned long f[94], g[94];

    /* openat s guest absolutni cestou -> prelozena, slot drzen */
    mkframe(f, 56, (unsigned long)-100, (unsigned long)"/etc/x", 0, 0);
    l1_before(f, 56);
    CHECK(!strcmp((const char *)f[23], "/data/R/etc/x"), "openat: argument syscallu prelozen");
    CHECK(!strcmp((const char *)f[1], "/etc/x"), "openat: puvodni frame[1] nedotcen");
    CHECK(f[21] > L1_NEED_AFTER, "openat: slot drzen");
    CHECK(f[21] == (unsigned long)&g_l1_slots[0], "jednovlaknovy proces -> slot 0");

    /* zanoreni (signal uprostred stubu) -> jiny slot */
    mkframe(g, 79, (unsigned long)-100, (unsigned long)"/usr/y", 0, 0);
    l1_before(g, 79);
    CHECK(g[21] == (unsigned long)&g_l1_slots[1], "zanoreny stub dostal slot 1");
    CHECK(!strcmp((const char *)g[23], "/data/R/usr/y"), "zanoreny preklad nezasahl do slotu 0");
    CHECK(!strcmp((const char *)f[23], "/data/R/etc/x"), "slot 0 porad drzi prvni preklad");
    g[20] = 0; l1_after(g, 79);
    CHECK(g_l1_owner[1] == 0 && g[21] == 0, "l1_after uvolnil slot 1");
    f[20] = 3; l1_after(f, 56);
    CHECK(g_l1_owner[0] == 0, "l1_after uvolnil slot 0");

    /* rychla cesta: relativni cesta pres dirfd -> zadny slot, l1_after se nevola */
    mkframe(f, 79, 5, (unsigned long)"ls", 0, 0);
    l1_before(f, 79);
    CHECK(f[21] == 0, "relativni cesta: frame[21]=0 (zadny slot, zadny l1_after)");
    CHECK(f[23] == (unsigned long)"ls" || !strcmp((const char *)f[23], "ls"), "relativni cesta nezmenena");

    /* idempotence: uz prelozena cesta */
    mkframe(f, 56, (unsigned long)-100, (unsigned long)"/data/R/etc/x", 0, 0);
    l1_before(f, 56);
    CHECK(!strcmp((const char *)f[23], "/data/R/etc/x"), "idempotence: cesta pod ROOTFS se neprelozi znovu");
    l1_after(f, 56);

    /* host cesta (/proc) zustava */
    mkframe(f, 56, (unsigned long)-100, (unsigned long)"/proc/self/maps", 0, 0);
    l1_before(f, 56);
    CHECK(!strcmp((const char *)f[23], "/proc/self/maps"), "/proc zustava host");
    l1_after(f, 56);

    /* renameat2: dve cesty (x1, x3) */
    mkframe(f, 276, (unsigned long)-100, (unsigned long)"/a", (unsigned long)-100, (unsigned long)"/b");
    l1_before(f, 276);
    CHECK(!strcmp((const char *)f[23], "/data/R/a") && !strcmp((const char *)f[25], "/data/R/b"),
          "renameat2: obe cesty prelozene do ruznych bufferu");
    l1_after(f, 276);

    /* symlinkat: cil (x0) se NEPREKLADA, jen linkpath (x2); absolutni cil
     * -> kanonicky relativni */
    mkframe(f, 36, (unsigned long)"/usr/bin/mawk", (unsigned long)-100,
            (unsigned long)"/etc/alternatives/awk", 0);
    l1_before(f, 36);
    CHECK(!strcmp((const char *)f[24], "/data/R/etc/alternatives/awk"), "symlinkat: linkpath prelozen");
    CHECK(!strcmp((const char *)f[22], "../../usr/bin/mawk"), "symlinkat: absolutni cil -> relativni");
    l1_after(f, 36);

    /* getcwd: strip prefixu vcetne NUL */
    char cwd[64] = "/data/R/home/u";
    mkframe(f, 17, (unsigned long)cwd, sizeof cwd, 0, 0);
    l1_before(f, 17);
    CHECK(f[21] == L1_NEED_AFTER, "getcwd: l1_after potreba bez slotu");
    f[20] = strlen(cwd) + 1;
    l1_after(f, 17);
    CHECK(!strcmp(cwd, "/home/u") && (long)f[20] == 8, "getcwd: /data/R/home/u -> /home/u (delka s NUL)");
    char cwd2[64] = "/data/R";
    mkframe(f, 17, (unsigned long)cwd2, sizeof cwd2, 0, 0);
    l1_before(f, 17); f[20] = strlen(cwd2) + 1; l1_after(f, 17);
    CHECK(!strcmp(cwd2, "/") && (long)f[20] == 2, "getcwd: presne koren -> \"/\"");
    char cwd3[64] = "/data/Rx/y";
    mkframe(f, 17, (unsigned long)cwd3, sizeof cwd3, 0, 0);
    l1_before(f, 17); f[20] = strlen(cwd3) + 1; l1_after(f, 17);
    CHECK(!strcmp(cwd3, "/data/Rx/y"), "getcwd: /data/Rx (jiny adresar) se nestripuje");
    g_cwd_keep_host = 1;
    char cwd4[64] = "/data/R/home";
    mkframe(f, 17, (unsigned long)cwd4, sizeof cwd4, 0, 0);
    l1_before(f, 17); f[20] = strlen(cwd4) + 1; l1_after(f, 17);
    CHECK(!strcmp(cwd4, "/data/R/home"), "getcwd: Bun (g_cwd_keep_host) -> host cesta");
    g_cwd_keep_host = 0;

    /* getcwd pod remap bindem -> guest mountpoint, ne strip $ROOTFS */
    char cwd5[64] = "/data/R/srv/b/sub";
    mkframe(f, 17, (unsigned long)cwd5, sizeof cwd5, 0, 0);
    l1_before(f, 17); f[20] = strlen(cwd5) + 1; l1_after(f, 17);
    CHECK(!strcmp(cwd5, "/mnt/b/sub") && (long)f[20] == 11, "getcwd: host cil bindu -> /mnt/b/sub");

    /* ro bind / mountpoint: l1_before syscall odmitne (vysledek ve frame[20],
     * zadny slot, argument neprelozen) */
    mkframe(f, 34, (unsigned long)-100, (unsigned long)"/ro/x", 0755, 0);
    long dn = l1_before(f, 34);
    CHECK(dn != 0 && (long)f[20] == -30 && f[21] == 0 && !strcmp((const char *)f[23], "/ro/x"),
          "mkdirat pod ro bindem -> odmitnuto -EROFS bez prekladu a slotu");
    CHECK(g_l1_owner[0] == 0, "odmitnuti: zadny slot drzen");
    mkframe(f, 35, (unsigned long)-100, (unsigned long)"/mnt/b", 0x200, 0);
    dn = l1_before(f, 35);
    CHECK(dn != 0 && (long)f[20] == -16, "unlinkat mountpointu -> -EBUSY");
    mkframe(f, 56, (unsigned long)-100, (unsigned long)"/proc/self/cmdline", 0, 0);
    dn = l1_before(f, 56);
    CHECK(dn != 0 && (long)f[20] == 0 && f[21] == 0, "openat /proc/self/cmdline -> obslouzeno, fd 0 je platny vysledek");
    mkframe(f, 34, (unsigned long)-100, (unsigned long)"/etc/x", 0755, 0);
    dn = l1_before(f, 34);
    CHECK(dn == 0 && !strcmp((const char *)f[23], "/data/R/etc/x"), "mkdirat mimo bind -> preklad");
    l1_after(f, 34);

    /* readlinkat: hostove absolutni cil (.l2s) -> strip, bez NUL */
    char lb[64]; memcpy(lb, "/data/R/.l2s/x", 14);
    mkframe(f, 78, (unsigned long)-100, (unsigned long)"/etc/l", (unsigned long)lb, sizeof lb);
    l1_before(f, 78);
    f[20] = 14; l1_after(f, 78);
    CHECK((long)f[20] == 7 && !memcmp(lb, "/.l2s/x", 7), "readlinkat: /data/R/.l2s/x -> /.l2s/x");
    CHECK(g_l1_owner[0] == 0, "readlinkat: slot uvolnen");

    /* readlinkat /proc/self/exe -> guest binarka */
    char eb[64];
    mkframe(f, 78, (unsigned long)-100, (unsigned long)"/proc/self/exe", (unsigned long)eb, sizeof eb);
    l1_before(f, 78); f[20] = 10; l1_after(f, 78);
    CHECK((long)f[20] == (long)strlen(g_guest_exe_path) && !memcmp(eb, g_guest_exe_path, f[20]),
          "readlinkat /proc/self/exe -> guest binarka");

    /* chyba syscallu se nemeni */
    char eb2[64];
    mkframe(f, 78, (unsigned long)-100, (unsigned long)"/etc/l", (unsigned long)eb2, sizeof eb2);
    l1_before(f, 78); f[20] = (unsigned long)-22; l1_after(f, 78);
    CHECK((long)f[20] == -22, "chybovy vysledek nezmenen");

    /* nepathovy syscall -> nic */
    mkframe(f, 63, 0, 0, 0, 0);
    l1_before(f, 63);
    CHECK(f[21] == 0, "read(63): nic, l1_after se nevola");

    /* vycerpani slotu: vsechny obsazene -> preklad se preskoci, zadny pad */
    for (int i = 0; i < L1_SLOTS; i++) g_l1_owner[i] = 0xdead;
    mkframe(f, 56, (unsigned long)-100, (unsigned long)"/etc/x", 0, 0);
    l1_before(f, 56);
    CHECK(f[21] == 0 && !strcmp((const char *)f[23], "/etc/x") && g_l1_miss == 1,
          "plna tabulka: bez prekladu, g_l1_miss++");
    for (int i = 0; i < L1_SLOTS; i++) g_l1_owner[i] = 0;

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
