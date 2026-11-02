/* binds_parse_spec_test.c — host-only test pro bind_parse_spec() v src/binds.h.
 *
 * Regrese 2026-11-02: boot posila `-b /product`, `-b /system`, `-b /apex`, ...
 * (identity bind bez dvojtecky, presne jak to dela proot). Puvodni
 * bind_parse_spec() takovy spec TISE ZAHODIL (`if (!colon) return;`), takze
 * guest pod loaderem --proot-compat nevidel /product, i kdyz ho proot videl.
 *
 * Build: gcc -I../src -o /tmp/bps_test binds_parse_spec_test.c && /tmp/bps_test
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ---- minimalni stuby zavislosti src/binds.h (host TP, libc OK) -------- */
static size_t shim_strlen(const char *s) { return strlen(s); }
static int shim_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static void *shim_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static int shim_excluded(const char *p) { (void)p; return 0; }
static const char *g_shim_root = "/tmp/bps_root";
static int elf_debug(void) { return 0; }
static int shim_mount(const char *a, const char *b, const char *c,
                       unsigned long d, const void *e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; return -1;
}
static char **shim_child_envp(char *const envp[]) { return (char **)envp; }
static const char *shim_strip_root(const char *p) { return p; }

#include "../src/binds.h"

/* najdi bind podle guest jmena; vraci index nebo -1 */
static int find_guest(const char *guest) {
    for (unsigned int i = 0; i < g_tab->count && i < LX_BIND_MAX; i++)
        if (g_tab->e[i].active && strcmp(g_tab->e[i].guest, guest) == 0)
            return (int)i;
    return -1;
}

static void reset_table(void) {
    /* nova lokalni tabulka bez dedeni session */
    unsetenv("LX_BINDS_FILE");
    unsetenv("LX_BINDS");
    g_tab = &g_tab_local;
    memset(g_tab, 0, sizeof *g_tab);
    g_tab->magic = LX_BIND_MAGIC;
    g_tab->root = binds_hash(g_shim_root);
    g_binds_ready = 1;
}

int main(void) {
    /* 1) IDENTITY bind bez dvojtecky — presne tvar, ktery posila boot. */
    reset_table();
    bind_parse_spec("/product");
    int i = find_guest("/product");
    assert(i >= 0 && "identity bind /product se zahodil");
    assert(strcmp(g_tab->e[i].host, "/product") == 0);
    assert(g_tab->e[i].ro == 0);
    printf("OK: '/product' -> host='/product' guest='/product' (identity)\n");

    /* 2) vic identity bindu za sebou (boot jich posila ~10) */
    const char *ids[] = { "/system", "/apex", "/odm", "/vendor",
                          "/system_ext", "/data/app",
                          "/data/dalvik-cache", "/linkerconfig/ld.config.txt" };
    for (size_t k = 0; k < sizeof ids / sizeof ids[0]; k++) {
        bind_parse_spec(ids[k]);
        int j = find_guest(ids[k]);
        assert(j >= 0);
        assert(strcmp(g_tab->e[j].host, ids[k]) == 0);
    }
    printf("OK: %zu identity bindu z bootu vsechny v tabulce\n",
           sizeof ids / sizeof ids[0]);

    /* 3) remap s dvojteckou funguje dal */
    reset_table();
    bind_parse_spec("/host/src:/guest/dst");
    int j = find_guest("/guest/dst");
    assert(j >= 0);
    assert(strcmp(g_tab->e[j].host, "/host/src") == 0);
    printf("OK: remap '/host/src:/guest/dst' funguje\n");

    /* 4) remap s ro flagem */
    reset_table();
    bind_parse_spec("/host/src:/guest/dst:ro");
    j = find_guest("/guest/dst");
    assert(j >= 0 && g_tab->e[j].ro == 1);
    printf("OK: ro flag se prenese\n");

    /* 5) prazdny a komentar se ignoruji */
    reset_table();
    unsigned int before = g_tab->count;
    bind_parse_spec("");
    bind_parse_spec("# komentar");
    bind_parse_spec(NULL);
    assert(g_tab->count == before);
    printf("OK: prazdny/komentar/NULL se ignoruji\n");

    /* 6) identita s ro flagem (":ro" bez host casti nejde, ale /a:/a:ro ano) */
    reset_table();
    bind_parse_spec("/x:/x:ro");
    j = find_guest("/x");
    assert(j >= 0 && g_tab->e[j].ro == 1);
    printf("OK: identity s ro '/x:/x:ro'\n");

    printf("ALL PASS\n");
    return 0;
}
