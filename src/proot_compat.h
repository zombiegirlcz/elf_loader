/* src/proot_compat.h — prompt 09: proot-kompatibilni CLI + hybridni dispatch.
 *
 * Cil: appka (ProotManager.kt) umi vymenit "proot" za "elf_loader" jen
 * zamenou binarky. Dve casti:
 *
 *  1. `elf_loader --proot-compat <proot argv>` (nebo detekce argv[0]==proot,
 *     viz main()) — rozparsuje typickou proot syntaxi (-r/-b/-w/-0/-l/-k/
 *     --kill-on-exit), promitne ji do ROOTFS/LX_BINDS/LX_FAKEROOT/LX_L2S a
 *     zbytek argv dispatchne jako --ownall <cmd>. Nezname volby (proot umi
 *     desitky dalsich, napr. -S/-v/-V/--sysvipc) -> cele puvodni volani se
 *     beze zmeny preda REALNEMU prootu (LX_PROOT_BIN), aby appka nikdy
 *     nedostala "unknown option" na necem, co proot-compat jeste nepokryva.
 *
 *  2. Hybridni dispatch v exec ceste (main.c shim_execve / shim_posix_spawnp):
 *     binarka, kterou loader neumi (staticka glibc/jina e_machine bez L2,
 *     nebo basename v $ROOTFS/etc/lx-force-proot) se spusti pod REALNYM
 *     prootem se stejnymi bindy, misto primeho real execve (ktery by cestu
 *     vubec nepreložil a binarka by nevidela rootfs).
 *
 * Bezpecne pod guest TP: pouziva jen shim_str... a raw_... a getenv/setenv, ktere
 * uz main.c pouziva na stejne horke ceste (POZOR: setenv se vola jen z
 * hlavniho vlakna pred elf_run, nikdy z hot path shim_execve pod guest TP -
 * tam se jen CTE env, nic se nenastavuje).
 */
#ifndef ELF_LOADER_PROOT_COMPAT_H
#define ELF_LOADER_PROOT_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/* ---- realny proot: cesta -------------------------------------------- */

/* LX_PROOT_BIN > bezne instalacni cesty > "proot" v $PATH. Volano jen z
 * hlavniho vlakna (bionicky TP, getenv/access OK). */
static const char *proot_compat_real_bin(void) {
    const char *p = getenv("LX_PROOT_BIN");
    if (p && p[0]) return p;
    static const char *cands[] = {
        "/data/data/com.linux_core/files/usr/bin/proot",
        "/data/user/0/com.linux_core/files/usr/bin/proot",
        "/data/adb/elf_loader/proot",
        "/system/bin/proot",
        NULL
    };
    for (int i = 0; cands[i]; i++)
        if (access(cands[i], X_OK) == 0) return cands[i];
    return "proot";
}

/* ---- sber aktivnich bindu z binds.h tabulky pro `proot -b` ----------- */

/* Vyplni argv polozky "-b" "host:guest"[:ro] z g_tab (pokud existuje) nebo
 * z LX_BINDS env (fallback, kdyz tabulka jeste neni inicializovana). Vraci
 * pocet pouzitych slotu v out (po dvojicich -b/host:guest). max = pocet
 * volnych slotu v out (musi byt sudy). */
static int proot_compat_collect_binds(char **out, int max, char *scratch, size_t scratch_sz) {
    int n = 0;
    size_t used = 0;
#ifdef ELF_LOADER_BINDS_H
    if (g_tab) {
        for (unsigned i = 0; i < g_tab->count && n + 2 <= max; i++) {
            lx_bind_t *e = &g_tab->e[i];
            if (!e->active) continue;
            /* identity bindy (guest==host, default exclude-list migrace)
             * nedavaji realnemu prootu nic navic (ten je uvnitř chrootu
             * identicky) a jen by zahltily argv -> vynechat. */
            if (e->glen == e->hlen && strncmp(e->guest, e->host, e->glen) == 0)
                continue;
            size_t need = (size_t)e->hlen + 1 + (size_t)e->glen + (e->ro ? 3 : 0) + 1;
            if (used + need > scratch_sz) break;
            char *dst = scratch + used;
            memcpy(dst, e->host, e->hlen);
            dst[e->hlen] = ':';
            memcpy(dst + e->hlen + 1, e->guest, e->glen);
            size_t p = (size_t)e->hlen + 1 + (size_t)e->glen;
            if (e->ro) { memcpy(dst + p, ":ro", 3); p += 3; }
            dst[p] = 0;
            used += p + 1;
            out[n++] = (char *)"-b";
            out[n++] = dst;
        }
        return n;
    }
#endif
    const char *env = getenv("LX_BINDS");
    if (!env || !env[0]) return 0;
    size_t len = strlen(env);
    if (len + 1 > scratch_sz) len = scratch_sz - 1;
    memcpy(scratch, env, len);
    scratch[len] = 0;
    char *tok = scratch;
    while (tok && *tok && n + 2 <= max) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = 0;
        if (*tok) {
            out[n++] = (char *)"-b";
            out[n++] = tok;
        }
        tok = comma ? comma + 1 : NULL;
    }
    return n;
}

/* ---- cizi architektura: qemu-<arch>-static v rootfs (proot -q) -------- */

/* EM_* -> qemu-user arch suffix (jen bezne pripady, zbytek NULL -> real
 * proot bez -q, proot pak selha sam, pokud binarku neumi). */
static const char *proot_compat_qemu_arch(unsigned short mach) {
    switch (mach) {
        case 3:   return "i386";
        case 62:  return "x86_64";
        case 40:  return "arm";
        case 183: return "aarch64";
        case 20:  return "ppc";
        case 21:  return "ppc64";
        case 243: return "riscv64";
        case 8:   return "mips";
        case 22:  return "s390x";
        default:  return NULL;
    }
}

/* Cesta k $ROOTFS/usr/bin/qemu-<arch>-static, pokud existuje; jinak NULL. */
static const char *proot_compat_qemu_bin(const char *rootfs, unsigned short mach) {
    static char path[4096];
    const char *arch = proot_compat_qemu_arch(mach);
    if (!arch || !rootfs || !rootfs[0]) return NULL;
    snprintf(path, sizeof path, "%s/usr/bin/qemu-%s-static", rootfs, arch);
    if (access(path, X_OK) == 0) return path;
    return NULL;
}

/* ---- exec realneho prootu (process replace, nevraci se na uspech) ---- */

/* rootfs: -r cil; extra_argv/extra_argc: dalsi proot-only volby (napr. -0,
 * -w cwd), uz hotove jako argv dvojice; cmd_argv: guest prikaz + jeho args
 * (cmd_argv[0] = binarka). envp se preda beze zmeny (real proot si LD_* a
 * spol. resi sam). Pri chybe execve vraci -1 a nechava errno. */
static int proot_compat_exec_real(const char *rootfs, char *const *extra_argv,
                                   int extra_argc, const char *path,
                                   char *const *argv, char *const envp[]) {
    char scratch[8192];
    char *bindv[2 * 64];
    int nbind = proot_compat_collect_binds(bindv, (int)(sizeof(bindv) / sizeof(bindv[0])),
                                            scratch, sizeof scratch);

    char *na[512];
    int narg = 0;
    const char *realproot = proot_compat_real_bin();
    na[narg++] = (char *)realproot;
    if (rootfs && rootfs[0]) {
        na[narg++] = (char *)"-r";
        na[narg++] = (char *)rootfs;
    }
    for (int i = 0; i < extra_argc && narg < 500; i++)
        na[narg++] = extra_argv[i];
    for (int i = 0; i < nbind && narg < 500; i++)
        na[narg++] = bindv[i];
    na[narg++] = (char *)"--";
    /* cesta jako guest-prikaz argv[0] (muze se lisit od puvodniho argv[0] -
     * exec*() dovoluje argv[0] != cesta); zbytek z puvodniho argv[1..]. */
    na[narg++] = (char *)path;
    for (int i = 1; argv && argv[i] && narg < 510; i++)
        na[narg++] = argv[i];
    na[narg] = NULL;

    execve(realproot, na, envp ? envp : environ);
    return -1;
}

/* ---- konfigurace "spust pod realnym prootem, ne pod loaderem" -------- */

/* basename v $ROOTFS/etc/lx-force-proot (jeden nazev na radek, #-komentare
 * a prazdne radky se preskakuji) -> i glibc dynamicka binarka, kterou by
 * loader jinak own-loadnul, se nasili posle pod realny proot. Pro binarky,
 * ktere loader own-loadovat NEUMI (staticke bez L2, cizi e_machine), se
 * stejny proot fallback pouzije bez ohledu na tenhle soubor (viz
 * elf_hybrid_needs_real_proot v main.c). */
static int proot_compat_is_forced(const char *rootfs, const char *basename_) {
    if (!rootfs || !rootfs[0] || !basename_ || !basename_[0]) return 0;
    char path[4096];
    snprintf(path, sizeof path, "%s/etc/lx-force-proot", rootfs);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int hit = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == 0) continue;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (*s && strcmp(s, basename_) == 0) { hit = 1; break; }
    }
    fclose(f);
    return hit;
}

/* ---- proot-compat CLI parser ------------------------------------------ */

/* Rozparsuje proot-kompatibilni volby v argv[0..argc). Pri uspechu vraci 0,
 * naplni *cmd_start indexem prvniho argumentu guest prikazu (muze byt ==
 * argc -> zadny prikaz, defaultni shell) a nastavi ROOTFS/LX_BINDS/
 * LX_FAKEROOT/LX_L2S env pro aktualni proces (dedi se do --ownall dispatche
 * stejne jako jinde). Pri nezname volbe nebo chybejicim -r vraci -1 (volajici
 * ma predat CELE puvodni argv realnemu prootu beze zmeny). */
static int proot_compat_parse(int argc, char **argv, int *cmd_start,
                               int *out_root_id) {
    static char binds_acc[4096];
    size_t binds_len = 0;
    const char *rootfs = NULL;
    const char *cwd = NULL;
    int root_id = 0, l2s = 0;
    int i = 0;
    binds_acc[0] = 0;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a) break;
        if ((strcmp(a, "-r") == 0 || strcmp(a, "--rootfs") == 0) && i + 1 < argc) {
            rootfs = argv[++i];
        } else if (strncmp(a, "--rootfs=", 9) == 0) {
            rootfs = a + 9;
        } else if ((strcmp(a, "-b") == 0 || strcmp(a, "--bind") == 0) && i + 1 < argc) {
            const char *b = argv[++i];
            size_t bl = strlen(b);
            if (binds_len + bl + 2 < sizeof binds_acc) {
                if (binds_len) binds_acc[binds_len++] = ',';
                memcpy(binds_acc + binds_len, b, bl);
                binds_len += bl;
                binds_acc[binds_len] = 0;
            }
        } else if (strncmp(a, "--bind=", 7) == 0) {
            const char *b = a + 7;
            size_t bl = strlen(b);
            if (binds_len + bl + 2 < sizeof binds_acc) {
                if (binds_len) binds_acc[binds_len++] = ',';
                memcpy(binds_acc + binds_len, b, bl);
                binds_len += bl;
                binds_acc[binds_len] = 0;
            }
        } else if ((strcmp(a, "-w") == 0 || strcmp(a, "--cwd") == 0 ||
                    strcmp(a, "--pwd") == 0) && i + 1 < argc) {
            cwd = argv[++i];
        } else if (strncmp(a, "--cwd=", 6) == 0) {
            cwd = a + 6;
        } else if (strcmp(a, "-0") == 0 || strcmp(a, "--root-id") == 0) {
            root_id = 1;
        } else if (strcmp(a, "-l") == 0 || strcmp(a, "--link2symlink") == 0) {
            l2s = 1;
        } else if ((strcmp(a, "-k") == 0 || strcmp(a, "--kernel-release") == 0) &&
                   i + 1 < argc) {
            i++;  /* loader kernel-release neemuluje, hodnotu jen spolknout */
        } else if (strncmp(a, "--kernel-release=", 17) == 0) {
            /* ignorovat */
        } else if (strcmp(a, "--kill-on-exit") == 0) {
            /* loader dite nemusi explicitne hlidat (zivotnost == loader
             * proces), no-op pro kompatibilitu CLI. */
        } else if (strcmp(a, "--") == 0) {
            i++;
            break;
        } else if (a[0] != '-') {
            break;  /* zacatek guest prikazu */
        } else {
            return -1;  /* nezname volby (-S, -v, -V, --sysvipc, ...) */
        }
    }
    if (!rootfs || !rootfs[0]) return -1;

    setenv("ROOTFS", rootfs, 1);
    if (binds_len) setenv("LX_BINDS", binds_acc, 1);
    if (root_id) setenv("LX_FAKEROOT", "1", 1);
    if (l2s) setenv("LX_L2S", "1", 1);
    if (cwd && cwd[0]) setenv("ELF_LOADER_PROOT_CWD", cwd, 1);
    if (out_root_id) *out_root_id = root_id;
    *cmd_start = i;
    return 0;
}

#endif /* ELF_LOADER_PROOT_COMPAT_H */
