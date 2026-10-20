/* src/relsym.h — helpery promptu 03 (absolutni symlinky -> kanonicky
 * relativni tvar, clamp ".." nad korenem).
 *
 * Vklada se PRIMO do main.c (nad shim_translate), protoze pouziva jeho
 * TLS-safe primitiva (shim_str*, shim_raw_syscall6, shim_excluded,
 * g_shim_root) a bezi pod GUEST TP. Samostatny soubor je jen proto, aby
 * sla stejna logika otestovat na hostu (test/relsym_test.c). */
#ifndef ELF_LOADER_RELSYM_H
#define ELF_LOADER_RELSYM_H

/* ===== Prompt 03: absolutni symlinky, kanonicky relativni tvar, clamp ".."
 *
 * Jadro vyhodnocuje absolutni symlink uvnitr ROOTFS proti HOST koreni
 * (/etc/alternatives/awk -> /usr/bin/mawk mysli HOST /usr/bin). Invariant,
 * ktery drzime: takove linky jsou v rootfs ulozene RELATIVNE v "kanonickem
 * tvaru" = presne hloubka x "../" + cesta od guest korene. Jadro je pak
 * uvnitr rootfs vyhodnoti spravne a za nulovou cenu za behu (zadny shim).
 *   - jednorazovou konverzi dela tools/relsymlinks.sh,
 *   - invariant udrzuje shim_symlink/shim_symlinkat (absolutni cil zapise
 *     rovnou relativne) - dpkg, update-alternatives, `ln -s /x`,
 *   - shim_readlink/shim_readlinkat hlasi kanonicky relativni cil zpet jako
 *     guest-absolutni, protoze update-alternatives cile porovnava.
 * Riziko heuristiky u readlinku: skutecny relativni link presne tohoto tvaru
 * se ohlasi jako absolutni - uvnitr korene je to semanticky totez.
 * Knoby: ELF_LOADER_NO_RELSYM=1 (vypne 2+3), ELF_LOADER_NO_DOTDOT_CLAMP=1. */
static int g_no_relsym = 0;
static int g_no_dotdot_clamp = 0;

/* Guest cesta z hostove (pod ROOTFS): ".../parrot/etc/x" -> "/etc/x".
 * NULL = cesta neni pod ROOTFS. */
static const char *shim_guest_of(const char *host) {
    if (!host || !g_shim_root || !g_shim_root[0]) return NULL;
    size_t rl = shim_strlen(g_shim_root);
    if (shim_strncmp(host, g_shim_root, rl) != 0) return NULL;
    if (host[rl] == 0) return "/";
    if (host[rl] != '/') return NULL;
    return host + rl;
}

/* Hloubka adresare, ve kterem guest cesta lezi (pocet komponent dirname):
 * "/etc/alternatives/awk" -> 2, "/awk" -> 0. -1 = nesmysl. */
static int shim_guest_dir_depth(const char *g) {
    if (!g || g[0] != '/') return -1;
    int comps = 0;
    const char *p = g;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        comps++;
        while (*p && *p != '/') p++;
    }
    if (comps < 1) return -1;
    return comps - 1;
}

/* Hostova absolutni cesta i pro relativni argument (raw getcwd je TLS-safe).
 * Slouzi jen k urceni hloubky linku, ne jako argument syscallu. */
static int shim_abs_host_path(const char *p, char *out, size_t n) {
    if (!p || !p[0]) return 0;
    if (p[0] == '/') { shim_strcpy(out, n, p); return 1; }
    char cwd[4096];
    long r = shim_raw_syscall6(17 /* SYS_getcwd */, (long)cwd, (long)sizeof cwd, 0, 0, 0, 0);
    if (r <= 0) return 0;
    cwd[sizeof(cwd) - 1] = 0;
    shim_strcpy(out, n, cwd);
    size_t l = shim_strlen(out);
    if (l && out[l - 1] != '/') shim_strcat(out, n, "/");
    shim_strcat(out, n, p);
    return 1;
}

/* Absolutni guest cil -> kanonicky relativni tvar vuci adresari linku.
 * host_linkpath = jiz prelozena (hostova) cesta linku. 1 = out naplneno. */
static int shim_abs_target_to_rel(const char *target, const char *host_linkpath,
                                  char *out, size_t n) {
    if (g_no_relsym) return 0;
    if (!target || target[0] != '/') return 0;
    if (shim_excluded(target)) return 0;   /* host mountpointy nech absolutni */
    size_t rl = g_shim_root ? shim_strlen(g_shim_root) : 0;
    if (!rl) return 0;
    /* cil uz hostove-absolutni pod ROOTFS (proot .l2s) -> nechat */
    if (shim_strncmp(target, g_shim_root, rl) == 0 &&
        (target[rl] == '/' || target[rl] == 0)) return 0;
    char hp[8192];
    if (!shim_abs_host_path(host_linkpath, hp, sizeof hp)) return 0;
    const char *g = shim_guest_of(hp);
    if (!g) return 0;                      /* link neni pod ROOTFS */
    int depth = shim_guest_dir_depth(g);
    if (depth < 0) return 0;
    const char *rest = target;
    while (*rest == '/') rest++;
    if ((size_t)depth * 3 + shim_strlen(rest) + 2 > n) return 0;
    size_t o = 0;
    for (int i = 0; i < depth; i++) { out[o++] = '.'; out[o++] = '.'; out[o++] = '/'; }
    out[o] = 0;
    shim_strcat(out, n, rest);
    if (!*rest && o) out[o - 1] = 0;       /* cil = koren -> "../.." bez koncoveho / */
    if (!out[0]) shim_strcpy(out, n, ".");  /* link v koreni, cil koren */
    return 1;
}

/* Kanonicky relativni cil (presne depth x "../" + zbytek) -> guest-absolutni. */
static int shim_rel_target_to_abs(const char *target, const char *host_linkpath,
                                  char *out, size_t n) {
    if (g_no_relsym) return 0;
    if (!target || !target[0] || target[0] == '/') return 0;
    char hp[8192];
    if (!shim_abs_host_path(host_linkpath, hp, sizeof hp)) return 0;
    const char *g = shim_guest_of(hp);
    if (!g) return 0;
    int depth = shim_guest_dir_depth(g);
    if (depth <= 0) return 0;              /* hloubka 0 -> nelze rozlisit */
    const char *p = target;
    for (int i = 0; i < depth; i++) {
        if (!(p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0))) return 0;
        p += (p[2] == '/') ? 3 : 2;
    }
    if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) return 0;  /* vic ".." nez hloubka */
    if (shim_strlen(p) + 2 > n) return 0;
    out[0] = '/';
    shim_strcpy(out + 1, n - 1, p);
    return 1;
}

/* Clamp ".." nad guest korenem: v chrootu je "/.." == "/", ale prelozena
 * cesta by utekla nad ROOTFS (.../distro). Odstranujeme JEN ty ".."
 * komponenty, ktere by utekly; ostatni zustavaji jadru (plna lexikalni
 * normalizace by menila semantiku tam, kde je v ceste symlink).
 * 1 = out naplneno (neco bylo uzavreno), 0 = nechat puvodni. */
static int shim_clamp_dotdot(const char *p, char *out, size_t n) {
    if (g_no_dotdot_clamp || !p || p[0] != '/' || n < 2) return 0;
    int has = 0;
    for (const char *s = p + 1; s[0]; s++)
        if (s[-1] == '/' && s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == 0)) { has = 1; break; }
    if (!has) return 0;
    size_t o = 0;
    int depth = 0, dropped = 0;
    out[o++] = '/';
    const char *s = p;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t cl = (size_t)(e - s);
        if (cl == 1 && s[0] == '.') { s = e; continue; }
        if (cl == 2 && s[0] == '.' && s[1] == '.') {
            if (depth == 0) { dropped = 1; s = e; continue; }   /* clamp na koreni */
            depth--;
        } else depth++;
        if (o + cl + 2 > n) return 0;
        if (o > 1) out[o++] = '/';
        shim_memcpy(out + o, s, cl);
        o += cl;
        s = e;
    }
    out[o] = 0;
    return dropped;
}


#endif /* ELF_LOADER_RELSYM_H */
