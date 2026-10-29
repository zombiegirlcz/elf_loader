/* binds_gready_test.c — host-only doplnkovy test k nalezu #6 (proot-compat
 * bind_reverse). Cil: empiricky overit chovani bind_reverse() PRI
 * g_binds_ready == 0 (edge case zminovana v zadani), primo nad realnym
 * src/binds.h (ne nad stubem jako test/proot_compat_test.c).
 *
 * Build:  gcc -I../src -o /tmp/bg_test binds_gready_test.c && /tmp/bg_test
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ---- minimalni stuby zavislosti src/binds.h (host TP, libc OK) -------- */
static size_t shim_strlen(const char *s) { return strlen(s); }
static int shim_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static void *shim_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static int shim_excluded(const char *p) { (void)p; return 0; }
static const char *g_shim_root = "/tmp/binds_gready_root";
static int elf_debug(void) { return 0; }
static int shim_mount(const char *a, const char *b, const char *c,
                       unsigned long d, const void *e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; return -1;
}
static char **shim_child_envp(char *const envp[]) { return (char **)envp; }
static const char *shim_strip_root(const char *p) { return p; }

#include "../src/binds.h"

int main(void) {
    /* 1) binds_init() tak, aby tabulka mela skutecny bind, a potvrdime, ze
     *    bind_reverse() za normalnich okolnosti (g_binds_ready==1) funguje -
     *    kontrolni pozitivni test, aby selhani v kroku 2 neslo pripsat chybe
     *    v setupu. */
    setenv("ROOTFS", g_shim_root, 1);
    setenv("LX_BINDS", "/tmp/binds_gready_hostdir:/opt/x", 1);
    unsetenv("LX_BINDS_FILE");
    binds_init();
    assert(g_binds_ready == 1);

    char out[256];
    int ok = bind_reverse("/tmp/binds_gready_hostdir/sub/file", out, sizeof out);
    assert(ok == 1);
    assert(strcmp(out, "/opt/x/sub/file") == 0);
    printf("OK: bind_reverse() s g_binds_ready=1 najde remap -> %s\n", out);

    /* 2) EDGE CASE ze zadani: g_binds_ready == 0. V realnem binaru tohle
     *    nejde dosahnout (binds_init() v run_ownall() ho nastavi na 1
     *    nepodmineně PRED jakymkoli moznym volanim proot_compat_exec_real -
     *    viz rozbor v main.c: jedine dve call-site elf_hybrid_exec_proot()
     *    [main.c:2622, 2709] jsou uvnitr exec() shimu, ktery bezi jen
     *    v procesu, kde uz run_ownall() probehl). Testujeme tedy primo
     *    invariant, na kterem stoji bezpecnost fallbacku v
     *    proot_compat_exec_real: bind_reverse() MUSI bez ready-flagu vratit
     *    0 (ne spadnout, ne vratit nesmyslny vysledek), aby kod spravne
     *    spadl do kroku 2 (odriznuti ROOTFS prefixu). */
    g_binds_ready = 0;
    memset(out, 'X', sizeof out);  /* kanarek - overi, ze fce do out nic nenapsala */
    int ok2 = bind_reverse("/tmp/binds_gready_hostdir/sub/file", out, sizeof out);
    assert(ok2 == 0);
    assert(out[0] == 'X');  /* bind_reverse se vratila pred jakymkoli zapisem */
    printf("OK: bind_reverse() s g_binds_ready=0 vraci 0 bez zapisu do out "
           "(bezpecny fallback na krok 2 v proot_compat_exec_real)\n");

    /* 3) Restaurovat ready a potvrdit, ze se tabulka da znovu pouzit (neni
     *    to permanentni poskozeni stavu - relevantni, kdyby n kod nekdy
     *    docasne vypnul g_binds_ready a pak se vratil do normalniho behu). */
    g_binds_ready = 1;
    int ok3 = bind_reverse("/tmp/binds_gready_hostdir/sub/file", out, sizeof out);
    assert(ok3 == 1);
    assert(strcmp(out, "/opt/x/sub/file") == 0);
    printf("OK: po navratu g_binds_ready=1 bind_reverse() znovu funguje\n");

    printf("ALL PASS\n");
    return 0;
}
