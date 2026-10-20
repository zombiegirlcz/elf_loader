/* relsym_test.c — host unit test logiky promptu 03 (src/relsym.h):
 * absolutni cil symlinku -> kanonicky relativni tvar, zpetny prevod pro
 * readlink, clamp ".." nad guest korenem. Zadne zarizeni nepotreba.
 *
 * relsym.h se v main.c vklada pod GUEST TP a pouziva jeho TLS-safe
 * primitiva; tady je dodavame jako bezne funkce (vcetne shim_raw_syscall6
 * pro SYS_getcwd, ktery tu obsluhujeme z promenne g_fake_cwd).
 *
 * Build: cc -Wall -Iinclude -Isrc test/relsym_test.c -o /tmp/rt
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── primitiva, ktera relsym.h ocekava z main.c ────────────────────────── */
static const char *g_shim_root = "/data/user/0/com.linux_core/files/nh/distro/parrot";
static const char *g_fake_cwd = "/data/user/0/com.linux_core/files/nh/distro/parrot/etc";

static size_t shim_strlen(const char *s) { size_t i = 0; while (s[i]) i++; return i; }
static int shim_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static void shim_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static void shim_strcpy(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    if (!dstsz) return;
    while (src[i] && i < dstsz - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void shim_strcat(char *dst, size_t dstsz, const char *src) {
    size_t dl = shim_strlen(dst);
    if (dl >= dstsz) return;
    shim_strcpy(dst + dl, dstsz - dl, src);
}
static int shim_excluded(const char *p) {
    static const char *excl[] = {
        "/proc", "/dev", "/sys", "/system", "/data", "/apex", "/linkerconfig",
        "/metadata", "/sdcard", "/storage", "/vendor", "/odm", "/product",
        "/persist", "/cache", "/config", "/debug_ramdisk", NULL
    };
    for (int i = 0; excl[i]; i++) {
        size_t l = shim_strlen(excl[i]);
        if (shim_strncmp(p, excl[i], l) == 0 && (p[l] == '/' || p[l] == 0)) return 1;
    }
    return 0;
}
/* jen SYS_getcwd(17) - relsym.h nic jineho nevola */
static long shim_raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (nr != 17) return -1;
    char *buf = (char *)a0;
    size_t cap = (size_t)a1;
    size_t l = strlen(g_fake_cwd);
    if (l + 1 > cap) return -1;
    memcpy(buf, g_fake_cwd, l + 1);
    return (long)(l + 1);
}

#include "relsym.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else printf("ok: %s\n", msg); \
} while (0)

/* hostova cesta linku z guest cesty */
static const char *H(const char *guest) {
    static char b[4096];
    snprintf(b, sizeof b, "%s%s", g_shim_root, guest);
    return b;
}
static int a2r(const char *target, const char *guest_link, char *out, size_t n) {
    return shim_abs_target_to_rel(target, H(guest_link), out, n);
}
static int r2a(const char *target, const char *guest_link, char *out, size_t n) {
    return shim_rel_target_to_abs(target, H(guest_link), out, n);
}

int main(void) {
    char o[4096];

    /* ── 1. absolutni cil -> kanonicky relativni ───────────────────────── */
    CHECK(a2r("/usr/bin/mawk", "/etc/alternatives/awk", o, sizeof o) &&
          strcmp(o, "../../usr/bin/mawk") == 0, "/etc/alternatives/awk -> ../../usr/bin/mawk");
    CHECK(a2r("/usr/bin/x", "/bin/x", o, sizeof o) &&
          strcmp(o, "../usr/bin/x") == 0, "hloubka 1");
    CHECK(a2r("/etc/x", "/link", o, sizeof o) &&
          strcmp(o, "etc/x") == 0, "hloubka 0 (link v koreni)");
    CHECK(a2r("/", "/a/b/l", o, sizeof o) && strcmp(o, "../..") == 0,
          "cil = koren -> presne hloubka x ..");

    /* host cesty a mountpointy zustavaji absolutni */
    CHECK(!a2r("/system/lib64/libc.so", "/etc/l", o, sizeof o), "/system zustava absolutni");
    CHECK(!a2r("/proc/self/fd/1", "/etc/l", o, sizeof o), "/proc zustava absolutni");
    CHECK(!a2r("/data/x", "/etc/l", o, sizeof o), "/data zustava absolutni");
    /* cil uz hostove-absolutni pod ROOTFS (proot .l2s) */
    CHECK(!a2r(H("/usr/bin/x"), "/etc/l", o, sizeof o), "cil pod ROOTFS zustava");
    /* relativni cil se nekonvertuje */
    CHECK(!a2r("../usr/bin/x", "/etc/l", o, sizeof o), "relativni cil se nemeni");
    /* link mimo ROOTFS */
    CHECK(!a2r("/usr/bin/x", NULL, o, sizeof o) || 1, "NULL link neshodi");
    CHECK(!shim_abs_target_to_rel("/usr/bin/x", "/tmp/outside", o, sizeof o),
          "link mimo ROOTFS -> neprevadet");

    /* relativni link path: hloubka z cwd (g_fake_cwd = $R/etc) */
    CHECK(shim_abs_target_to_rel("/usr/bin/mawk", "alternatives/awk", o, sizeof o) &&
          strcmp(o, "../../usr/bin/mawk") == 0, "relativni linkpath pres cwd");

    /* ── 2. kanonicky relativni -> absolutni (readlink) ────────────────── */
    CHECK(r2a("../../usr/bin/mawk", "/etc/alternatives/awk", o, sizeof o) &&
          strcmp(o, "/usr/bin/mawk") == 0, "readlink: ../../usr/bin/mawk -> /usr/bin/mawk");
    CHECK(r2a("../usr/bin/x", "/bin/x", o, sizeof o) &&
          strcmp(o, "/usr/bin/x") == 0, "readlink: hloubka 1");
    /* ne-kanonicky tvar (mene/vic ".." nez hloubka) se nemeni */
    CHECK(!r2a("../usr/bin/x", "/etc/alternatives/awk", o, sizeof o),
          "readlink: mene .. nez hloubka -> nechat");
    CHECK(!r2a("../../../usr/bin/x", "/etc/alternatives/awk", o, sizeof o),
          "readlink: vic .. nez hloubka -> nechat");
    CHECK(!r2a("gawk", "/etc/alternatives/awk", o, sizeof o),
          "readlink: cil bez .. -> nechat");
    CHECK(!r2a("/usr/bin/x", "/etc/alternatives/awk", o, sizeof o),
          "readlink: absolutni cil -> nechat");
    CHECK(!r2a("x", "/link", o, sizeof o), "readlink: hloubka 0 -> nerozlisitelne, nechat");
    /* round trip */
    CHECK(a2r("/usr/lib/aarch64-linux-gnu/libz.so.1", "/usr/lib/x/y/libz.so", o, sizeof o),
          "roundtrip: prevod tam");
    {
        char back[4096];
        CHECK(r2a(o, "/usr/lib/x/y/libz.so", back, sizeof back) &&
              strcmp(back, "/usr/lib/aarch64-linux-gnu/libz.so.1") == 0,
              "roundtrip: a zpet stejny cil");
    }

    /* ── 3. clamp ".." nad korenem ─────────────────────────────────────── */
    CHECK(shim_clamp_dotdot("/../etc", o, sizeof o) && strcmp(o, "/etc") == 0,
          "clamp: /../etc -> /etc");
    CHECK(shim_clamp_dotdot("/..", o, sizeof o) && strcmp(o, "/") == 0,
          "clamp: /.. -> /");
    CHECK(shim_clamp_dotdot("/../../usr/bin", o, sizeof o) && strcmp(o, "/usr/bin") == 0,
          "clamp: /../../usr/bin -> /usr/bin");
    CHECK(shim_clamp_dotdot("/etc/../../usr", o, sizeof o) && strcmp(o, "/etc/../usr") == 0,
          "clamp: utekl jen druhy .., prvni zustava jadru");
    /* neutekajici ".." se NEMENI (jadro je resi spravne i se symlinky v ceste) */
    CHECK(!shim_clamp_dotdot("/etc/../usr", o, sizeof o), "clamp: /etc/../usr se nemeni");
    CHECK(!shim_clamp_dotdot("/usr/bin", o, sizeof o), "clamp: cesta bez .. se nemeni");
    CHECK(!shim_clamp_dotdot("/usr/..bin/x", o, sizeof o), "clamp: '..bin' neni '..'");
    CHECK(!shim_clamp_dotdot("/usr/bin/..", o, sizeof o), "clamp: koncove .. v hloubce se nemeni");

    /* ── 4. knoby vypnuti ──────────────────────────────────────────────── */
    g_no_relsym = 1;
    CHECK(!a2r("/usr/bin/mawk", "/etc/alternatives/awk", o, sizeof o),
          "ELF_LOADER_NO_RELSYM vypne prevod tam");
    CHECK(!r2a("../../usr/bin/mawk", "/etc/alternatives/awk", o, sizeof o),
          "ELF_LOADER_NO_RELSYM vypne prevod zpet");
    g_no_relsym = 0;
    g_no_dotdot_clamp = 1;
    CHECK(!shim_clamp_dotdot("/../etc", o, sizeof o), "ELF_LOADER_NO_DOTDOT_CLAMP vypne clamp");
    g_no_dotdot_clamp = 0;

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
