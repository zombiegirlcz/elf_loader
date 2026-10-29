/* src/proot_compat.h — prompt 09: proot-kompatibilni CLI + hybridni dispatch.
 *
 * Cil: appka (ProotManager.kt) umi vymenit "proot" za "elf_loader" jen
 * zamenou binarky. Dve casti:
 *
 *  1. `elf_loader --proot-compat <proot argv>` (nebo detekce argv[0]==proot,
 *     viz main()) — rozparsuje typickou proot syntaxi (-r/-b/-w/-0/-l/-k/
 *     -L/--change-id/--sysvipc/--kill-on-exit), promitne ji do ROOTFS/LX_BINDS/LX_FAKEROOT/LX_L2S a
 *     zbytek argv dispatchne jako --ownall <cmd>. Bezi pod BIONICKYM TP
 *     (hlavni vlakno pred elf_run), takze getenv/setenv/fopen/snprintf jsou
 *     bezpecne — tahle cast (proot_compat_parse, proot_compat_main v
 *     main.c) je muze volat normalne.
 *
 *  2. Hybridni dispatch v exec ceste (main.c shim_execve), volany PRIMO z
 *     guest kodu pod GUEST TP (parrot TLS): binarka, kterou loader neumi
 *     (cizi e_machine, basename v $ROOTFS/etc/lx-force-proot) se spusti
 *     pod REALNYM prootem se stejnymi bindy, misto primeho real execve.
 *     VSECHNY funkce v tehle casti (proot_compat_is_forced,
 *     proot_compat_qemu_bin, proot_compat_collect_binds,
 *     proot_compat_exec_real) jsou proto TLS-safe: jen raw_ a shim_str funkce
 *     syscally a staticka envp[] prohledavani (pc_envp_get), ZADNY
 *     getenv/setenv/fopen/snprintf/execve — presne pitfall #1 z AGENTS.md
 *     (guest TP nesmí volat bionicke TLS-zavisle funkce), uz 3x opakovany
 *     bug v tomhle projektu. g_shim_root/g_orig_execve jsou vyjimka: cachuji
 *     se jednou na startu pod bionickym TP, cteni je pak bezpecne odkudkoli.
 *
 * proot_compat.h se includuje do main.c AZ PO definici raw_open/raw_read/
 * raw_close/raw_access/shim_raw_syscall6 a fp_execve (jinak by hot-path cast
 * nemela, co volat).
 */
#ifndef ELF_LOADER_PROOT_COMPAT_H
#define ELF_LOADER_PROOT_COMPAT_H

/* ---- TLS-safe envp scan (pod guest TP bezpecne, zadny getenv) --------- */
static const char *pc_envp_get(char *const envp[], const char *name) {
    if (!envp) return NULL;
    size_t nl = shim_strlen(name);
    for (int i = 0; envp[i]; i++) {
        if (shim_strncmp(envp[i], name, nl) == 0 && envp[i][nl] == '=')
            return envp[i] + nl + 1;
    }
    return NULL;
}

/* ---- realny proot: cesta (TLS-safe varianta pro hybridni dispatch) ---- */
static const char *proot_compat_real_bin_envp(char *const envp[]) {
    const char *p = pc_envp_get(envp, "LX_PROOT_BIN");
    if (p && p[0]) return p;
    static const char *cands[] = {
        "/data/data/com.linux_core/files/usr/bin/proot",
        "/data/user/0/com.linux_core/files/usr/bin/proot",
        "/data/adb/elf_loader/proot",
        "/system/bin/proot",
        NULL
    };
    for (int i = 0; cands[i]; i++)
        if (raw_access(cands[i], 0 /* F_OK */) == 0) return cands[i];
    return "proot";
}

/* ---- sber aktivnich bindu z binds.h tabulky pro `proot -b` (TLS-safe) - */

/* Vyplni argv polozky "-b" "host:guest"[:ro] z g_tab (pokud existuje) nebo
 * z LX_BINDS v envp (fallback, kdyz tabulka jeste neni inicializovana).
 * Vraci pocet pouzitych slotu v out (po dvojicich -b/host:guest). max = pocet
 * volnych slotu v out (musi byt sudy). Jen shim_str a raw_ funkce — bezpecne pod
 * guest TP. */
static int proot_compat_collect_binds(char *const envp[], char **out, int max,
                                       char *scratch, size_t scratch_sz) {
    int n = 0;
#ifdef ELF_LOADER_BINDS_H
    size_t used = 0;
    if (g_tab) {
        for (unsigned i = 0; i < g_tab->count && n + 2 <= max; i++) {
            lx_bind_t *e = &g_tab->e[i];
            if (!e->active) continue;
            /* identity bindy (guest==host, default exclude-list migrace)
             * nedavaji realnemu prootu nic navic -> vynechat. */
            if (e->glen == e->hlen && shim_strncmp(e->guest, e->host, e->glen) == 0)
                continue;
            size_t need = (size_t)e->hlen + 1 + (size_t)e->glen + (e->ro ? 3 : 0) + 1;
            if (used + need > scratch_sz) break;
            char *dst = scratch + used;
            for (size_t k = 0; k < e->hlen; k++) dst[k] = e->host[k];
            dst[e->hlen] = ':';
            for (size_t k = 0; k < e->glen; k++) dst[e->hlen + 1 + k] = e->guest[k];
            size_t p = (size_t)e->hlen + 1 + (size_t)e->glen;
            if (e->ro) { dst[p] = ':'; dst[p+1] = 'r'; dst[p+2] = 'o'; p += 3; }
            dst[p] = 0;
            used += p + 1;
            out[n++] = (char *)"-b";
            out[n++] = dst;
        }
        return n;
    }
#endif
    const char *env = pc_envp_get(envp, "LX_BINDS");
    if (!env || !env[0]) return 0;
    size_t len = shim_strlen(env);
    if (len + 1 > scratch_sz) len = scratch_sz - 1;
    for (size_t k = 0; k < len; k++) scratch[k] = env[k];
    scratch[len] = 0;
    char *tok = scratch;
    while (tok && *tok && n + 2 <= max) {
        char *comma = shim_strchr(tok, ',');
        if (comma) *comma = 0;
        if (*tok) {
            out[n++] = (char *)"-b";
            out[n++] = tok;
        }
        tok = comma ? comma + 1 : NULL;
    }
    return n;
}

/* ---- cizi architektura: qemu-<arch>-static v rootfs (proot -q), TLS-safe */

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

/* Cesta k $ROOTFS/usr/bin/qemu-<arch>-static, pokud existuje; jinak NULL.
 * shim_str* misto snprintf (TLS-safe). */
static const char *proot_compat_qemu_bin(const char *rootfs, unsigned short mach) {
    static char path[4096];
    const char *arch = proot_compat_qemu_arch(mach);
    if (!arch || !rootfs || !rootfs[0]) return NULL;
    shim_strcpy(path, sizeof path, rootfs);
    shim_strcat(path, sizeof path, "/usr/bin/qemu-");
    shim_strcat(path, sizeof path, arch);
    shim_strcat(path, sizeof path, "-static");
    if (raw_access(path, 0 /* F_OK */) == 0) return path;
    return NULL;
}

/* ---- exec realneho prootu (process replace), TLS-safe ----------------- */

/* rootfs: -r cil; extra_argv/extra_argc: dalsi proot-only volby (napr. -q
 * qemu), uz hotove jako argv dvojice; path+argv: guest prikaz (path jako
 * argv[0] prootu, zbytek z argv[1..] — exec*() dovoluje argv[0] != cesta).
 * envp se preda beze zmeny. Pouziva g_orig_execve (bionicke, resolvovane
 * jednou pri startu pod bionickym TP) — ne primo execve() (ten by mohl byt
 * zaseklý v guest PLT/GOT pod F2, a i bionicky execve() wrapper z libc.so by
 * ho TLS-opatrny kod nemel volat primo pres jmeno pod guest TP). Pri chybe
 * vraci -1 (volajici ma pokracovat vlastnim fallbackem). */
static int proot_compat_exec_real(const char *rootfs, char *const *extra_argv,
                                   int extra_argc, const char *path,
                                   char *const *argv, char *const envp[]) {
    char scratch[8192];
    char *bindv[2 * 64];
    int nbind = proot_compat_collect_binds(envp, bindv,
                                            (int)(sizeof(bindv) / sizeof(bindv[0])),
                                            scratch, sizeof scratch);

    char *na[512];
    int narg = 0;
    const char *realproot = proot_compat_real_bin_envp(envp);
    na[narg++] = (char *)realproot;
    if (rootfs && rootfs[0]) {
        na[narg++] = (char *)"-r";
        na[narg++] = (char *)rootfs;
    }
    /* Emulace teto session prelozit na prootske volby. Bez toho dispatchovana
     * binarka bezi pod realnym uid/gid a s realnymi hardlinky, i kdyz session
     * jede pod fakerootem/l2s: chown/chmod na roota v maintainer scriptech
     * skonci na EPERM a link() na EACCES (Android hardlinky v app datech
     * zakazuje). Loaderovy libc shimy uvnitr prootu nebezi, takze samotne
     * LX_FAKEROOT/LX_L2S v envp nic neudelaji - musi to byt -0/-l.
     * pc_envp_get, ne getenv (guest TP). */
    const char *v_fr = pc_envp_get(envp, "LX_FAKEROOT");
    if (v_fr && v_fr[0] && !(v_fr[0] == '0' && v_fr[1] == 0))
        na[narg++] = (char *)"-0";
    const char *v_l2s = pc_envp_get(envp, "LX_L2S");
    if (v_l2s && v_l2s[0] && !(v_l2s[0] == '0' && v_l2s[1] == 0))
        na[narg++] = (char *)"-l";
    const char *v_cwd = pc_envp_get(envp, "ELF_LOADER_PROOT_CWD");
    if (v_cwd && v_cwd[0] == '/') {
        na[narg++] = (char *)"-w";
        na[narg++] = (char *)v_cwd;
    }
    for (int i = 0; i < extra_argc && narg < 500; i++)
        na[narg++] = extra_argv[i];
    for (int i = 0; i < nbind && narg < 500; i++)
        na[narg++] = bindv[i];
    /* ZADNE "--": proot z tohoto rootfs ho nezna ("proot error: unknown
     * option '--'") a hybridni dispatch na nem kvuli tomu nikdy neprosel.
     * Prikaz je vzdy absolutni cesta, takze separator neni potreba. */
    /* A proot chce GUEST cestu (ma uz -r rootfs), ne hostovou - jinak hlasi
     * "'<rootfs>/usr/sbin/ldconfig' not found (root = <rootfs>)". Rucni
     * smycky, zadne libc volani (guest TP). Poradi odvozeni:
     *   1) bind_reverse() - binarka dosazena pres bind remap (LX_BINDS) ma
     *      hostovou cestu mimo ROOTFS; bind_reverse ji prelozi na guest
     *      cestu v tabulce bindu (longest-prefix host->guest).
     *   2) odriznuti prefixu ROOTFS - binarka uz je pod ROOTFS.
     *   3) puvodni cesta beze zmeny - fallback. */
    char gpath_buf[4096];
    const char *gpath = path;
    if (path && bind_reverse(path, gpath_buf, sizeof gpath_buf)) {
        gpath = gpath_buf;
    } else if (rootfs && rootfs[0] && path) {
        size_t rl = 0;
        while (rootfs[rl]) rl++;
        while (rl > 1 && rootfs[rl - 1] == '/') rl--;  /* "<rootfs>/" */
        size_t i = 0;
        while (i < rl && path[i] == rootfs[i]) i++;
        if (i == rl && path[rl] == '/') gpath = path + rl;
    }
    na[narg++] = (char *)gpath;
    for (int i = 1; argv && argv[i] && narg < 510; i++)
        na[narg++] = argv[i];
    na[narg] = NULL;

    /* Realny proot potrebuje svuj PROOT_LOADER a PROOT_TMP_DIR. V prostredi
     * dpkg/maintainer-scriptu uvnitr guestu ani jedno neni (dpkg si env
     * ciste) a proot pak skonci na
     *   proot warning: can't canonicalize /tmp/
     *   proot error: execve("/usr/sbin/ldconfig"): No such file or directory
     * — tedy presne na triggeru `libc-bin`, kvuli kteremu dispatch vznikl.
     * Dolepime je do envp: loader = sourozenec prootovy binarky, tmp =
     * <rootfs>/tmp (hostovy /tmp na Androidu neexistuje). Staticke buffery:
     * tahle funkce uz jen execve, nevraci se. TLS-safe (zadne libc). */
    static char pl_buf[4200], pt_buf[4200];
    static char *ne[600];
    char *const *use_envp = envp;
    int have_pl = pc_envp_get(envp, "PROOT_LOADER") != NULL;
    int have_pt = pc_envp_get(envp, "PROOT_TMP_DIR") != NULL;
    if (!have_pl || !have_pt) {
        int o = 0;
        for (int i = 0; envp && envp[i] && o < 596; i++) ne[o++] = envp[i];
        if (!have_pl && o < 597) {
            shim_strcpy(pl_buf, sizeof pl_buf, "PROOT_LOADER=");
            size_t base = shim_strlen(pl_buf);
            shim_strcat(pl_buf, sizeof pl_buf, realproot);
            size_t cut = shim_strlen(pl_buf);
            while (cut > base && pl_buf[cut - 1] != '/') cut--;
            pl_buf[cut] = 0;
            shim_strcat(pl_buf, sizeof pl_buf, "loader");
            if (raw_access(pl_buf + base, 1 /*X_OK*/) == 0) ne[o++] = pl_buf;
        }
        if (!have_pt && o < 598 && rootfs && rootfs[0]) {
            shim_strcpy(pt_buf, sizeof pt_buf, "PROOT_TMP_DIR=");
            shim_strcat(pt_buf, sizeof pt_buf, rootfs);
            shim_strcat(pt_buf, sizeof pt_buf, "/tmp");
            ne[o++] = pt_buf;
        }
        ne[o] = NULL;
        use_envp = ne;
    }

    fp_execve f = (fp_execve)g_orig_execve;
    if (f) return f(realproot, na, use_envp);
    return (int)shim_raw_syscall6(SYS_execve, (long)realproot, (long)na, (long)use_envp, 0, 0, 0);
}

/* ---- konfigurace "spust pod realnym prootem, ne pod loaderem" (TLS-safe) */

/* basename v $ROOTFS/etc/lx-force-proot (jeden nazev na radek, #-komentare
 * a prazdne radky se preskakuji) -> i glibc dynamicka binarka, kterou by
 * loader jinak own-loadnul, se nasili posle pod realny proot. raw_open/
 * raw_read misto fopen/fgets (TLS-safe pod guest TP). */
static int proot_compat_is_forced(const char *rootfs, const char *basename_) {
    if (!rootfs || !rootfs[0] || !basename_ || !basename_[0]) return 0;
    char path[4096];
    shim_strcpy(path, sizeof path, rootfs);
    shim_strcat(path, sizeof path, "/etc/lx-force-proot");
    int fd = raw_open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = raw_read(fd, buf, sizeof buf - 1);
    raw_close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    size_t bl = shim_strlen(basename_);
    char *line = buf;
    int hit = 0;
    while (*line && !hit) {
        char *eol = shim_strchr(line, '\n');
        if (eol) *eol = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t sl = shim_strlen(s);
        while (sl > 0 && (s[sl-1] == '\r' || s[sl-1] == ' ' || s[sl-1] == '\t')) { s[--sl] = 0; }
        if (*s && *s != '#' && sl == bl && shim_strncmp(s, basename_, bl) == 0)
            hit = 1;
        line = eol ? eol + 1 : line + shim_strlen(line);
        if (!eol) break;
    }
    return hit;
}

/* ---- proot-compat CLI parser (bezi pod BIONICKYM TP, main()-time) ----- */

/* Rozparsuje proot-kompatibilni volby v argv[0..argc). Pri uspechu vraci 0,
 * naplni *cmd_start indexem prvniho argumentu guest prikazu (muze byt ==
 * argc -> zadny prikaz, defaultni shell) a nastavi ROOTFS/LX_BINDS/
 * LX_FAKEROOT/LX_L2S env pro aktualni proces (dedi se do --ownall dispatche
 * stejne jako jinde). Pri nezname volbe nebo chybejicim -r vraci -1 (volajici
 * ma predat CELE puvodni argv realnemu prootu beze zmeny). Volano jen z
 * main()/proot_compat_main — bionicky TP, getenv/setenv OK. */
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
        } else if (strcmp(a, "-L") == 0 || strcmp(a, "--fix-symlink-size") == 0) {
            /* proot timhle koriguje velikost, kterou lstat hlasi u symlinku
             * (nekompatibilni s nekterymi filesystemy); loader to nema a
             * chova se jako nativni beh — no-op pro kompatibilitu CLI. */
        } else if ((strcmp(a, "--change-id") == 0) && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "0:0") == 0) root_id = 1;
            else return -1;  /* loader umi jen fake root, ne libovolne uid:gid */
        } else if (strncmp(a, "--change-id=", 12) == 0) {
            const char *v = a + 12;
            if (strcmp(v, "0:0") == 0) root_id = 1;
            else return -1;
        } else if (strcmp(a, "--sysvipc") == 0) {
            /* no-op: zadna zname zavislost na System V IPC v tomhle projektu
             * (viz AGENTS.md); LX_PROOT_SYSVIPC=strict je escape hatch pro
             * kohokoliv, kdo real System V IPC potrebuje -> realny proot. */
            if (getenv("LX_PROOT_SYSVIPC") &&
                strcmp(getenv("LX_PROOT_SYSVIPC"), "strict") == 0)
                return -1;
        } else if (strcmp(a, "--") == 0) {
            i++;
            break;
        } else if (a[0] != '-') {
            break;  /* zacatek guest prikazu */
        } else {
            /* nezname volby (-S, -v, -V, ...) -> fallback na realny proot.
             * POZOR: --sysvipc sem uz NEDOJDE, ma vlastni vetev vys (no-op,
             * nebo -1 pri LX_PROOT_SYSVIPC=strict). */
            return -1;
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
