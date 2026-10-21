/* src/binds.h — L? (prompt 06): uzivatelska tabulka mountu / bindu.
 *
 * Nahrazuje natvrdo zapsany shim_excluded() longest-prefix tabulkou
 *   guest_prefix -> host_target [ro].
 * Vychozi obsah = dnesni exclude list jako IDENTITY bindy (guest==host), takze
 * default chovani je bajtove shodne. Navic pohlcuje /dev/shm special-case
 * (delsi prefix -> remap na $ROOTFS/dev/shm). Uzivatelske bindy prichazeji
 * z $ROOTFS/etc/lx-binds, env LX_BINDS (oboje format `host:guest[:ro]` jako
 * proot -b) a za behu z shim_mount (MS_BIND) — viz stage 2 (sdileny soubor).
 *
 * Vklada se do main.c (pod relsym.h/shim_translate primitiva). bind_kind/
 * bind_translate/bind_reverse bezi na HORKE ceste pod GUEST TP -> jen shim_str*
 * a zadne velke buffery na zasobniku. binds_init/binds_load_config bezi pri
 * startu pod bionickym TP (libc OK).
 */
#ifndef ELF_LOADER_BINDS_H
#define ELF_LOADER_BINDS_H

#define LX_BIND_MAX    64
#define LX_BIND_GLEN   256
#define LX_BIND_HLEN   512

typedef struct {
    unsigned char active;      /* 0 = volny slot (umount) */
    unsigned char ro;          /* 1 = read-only (zapis -> EROFS) */
    unsigned short glen, hlen; /* delky bez NUL */
    char guest[LX_BIND_GLEN];
    char host[LX_BIND_HLEN];
} lx_bind_t;

/* Stage 1: in-memory per-proces tabulka (naplni se ze startu + envu, deti ji
 * zdedi pres LX_BINDS/lx-binds, ktere znovu naparsuji). Stage 2 ji vymeni za
 * MAP_SHARED soubor kvuli runtime `mount`. */
static lx_bind_t g_binds[LX_BIND_MAX];
static int g_binds_n = 0;
static int g_binds_ready = 0;

/* bind_kind vysledky */
#define BIND_NONE     0   /* zadny bind -> default $ROOTFS prefix */
#define BIND_IDENTITY 1   /* host==guest (dnesni "excluded") -> nechat */
#define BIND_REMAP    2   /* host!=guest -> out naplneno remapem */

/* Komponentova shoda prefixu: `p` zacina `pref` a konci na hranici. pref=="/"
 * matchuje vse. Vraci delku shody (glen) nebo -1. */
static int bind_prefix_match(const char *p, const char *pref, int glen) {
    if (glen == 1 && pref[0] == '/') return (p[0] == '/') ? 1 : -1;
    if (shim_strncmp(p, pref, (size_t)glen) != 0) return -1;
    if (p[glen] == '/' || p[glen] == 0) return glen;
    return -1;
}

/* Najde nejdelsi matchujici aktivni bind pro guest `p`. Vraci index nebo -1. */
static int bind_longest(const char *p) {
    int best = -1, best_len = -1;
    for (int i = 0; i < g_binds_n; i++) {
        if (!g_binds[i].active) continue;
        int m = bind_prefix_match(p, g_binds[i].guest, g_binds[i].glen);
        if (m > best_len) { best_len = m; best = i; }
    }
    return best;
}

/* Jen druh shody, bez zapisu out (pro shim_excluded callery). */
static int bind_kind(const char *p) {
    if (!g_binds_ready) return BIND_NONE;
    int i = bind_longest(p);
    if (i < 0) return BIND_NONE;
    /* identity = host textove shodny s guest prefixem (tj. remap by nic nezmenil) */
    if (g_binds[i].glen == g_binds[i].hlen &&
        shim_strncmp(g_binds[i].guest, g_binds[i].host, g_binds[i].glen) == 0)
        return BIND_IDENTITY;
    return BIND_REMAP;
}

/* Overlap-safe kopie (clamp muze `p` nasmerovat dovnitr `out`). */
static void binds_move(char *d, const char *s, size_t n) {
    if (d == s || n == 0) return;
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else { for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1]; }
}

/* Prelozi guest `p` podle tabulky. Vraci BIND_NONE/IDENTITY/REMAP; pri REMAP
 * naplni out = host_target + zbytek za prefixem a *ro. */
static int bind_translate(const char *p, char *out, size_t n, int *ro) {
    if (ro) *ro = 0;
    if (!g_binds_ready) return BIND_NONE;
    int i = bind_longest(p);
    if (i < 0) return BIND_NONE;
    lx_bind_t *b = &g_binds[i];
    if (b->glen == b->hlen && shim_strncmp(b->guest, b->host, b->glen) == 0)
        return BIND_IDENTITY;
    const char *rest = p + b->glen;            /* zacina '/' nebo 0 */
    size_t restlen = shim_strlen(rest);
    if ((size_t)b->hlen + restlen + 1 > n) return BIND_NONE;  /* nevejde se -> default */
    /* `rest` muze lezet uvnitr `out` (clamp) -> nejdriv presun zbytek
     * (overlap-safe), pak zapis host prefix na zacatek (uz bez prekryvu). */
    binds_move(out + b->hlen, rest, restlen + 1);
    shim_memcpy(out, b->host, b->hlen);
    if (ro) *ro = b->ro;
    return BIND_REMAP;
}

/* Zpetne mapovani host->guest (getcwd/readlink): host pod nejakym remap
 * host_targetem -> guest_prefix + zbytek. Vraci 1 a naplni out, jinak 0.
 * Default $ROOTFS->/ resi shim_strip_root (identity bindy se neresi). */
static int bind_reverse(const char *host, char *out, size_t n) {
    if (!g_binds_ready || !host || host[0] != '/') return 0;
    int best = -1, best_len = -1;
    for (int i = 0; i < g_binds_n; i++) {
        lx_bind_t *b = &g_binds[i];
        if (!b->active) continue;
        if (b->glen == b->hlen && shim_strncmp(b->guest, b->host, b->glen) == 0)
            continue;                          /* identity: zadny remap */
        int m = bind_prefix_match(host, b->host, b->hlen);
        if (m > best_len) { best_len = m; best = i; }
    }
    if (best < 0) return 0;
    lx_bind_t *b = &g_binds[best];
    const char *rest = host + b->hlen;
    size_t restlen = shim_strlen(rest);
    if ((size_t)b->glen + restlen + 1 > n) return 0;
    shim_memcpy(out, b->guest, b->glen);
    shim_memcpy(out + b->glen, rest, restlen + 1);
    return 1;
}

/* ── naplneni tabulky (startup, bionic TP, libc OK) ──────────────────── */

/* Prida bind guest->host[:ro]. Vraci 1 ok. Deduplikuje podle guest. */
static int bind_add(const char *guest, const char *host, int ro) {
    if (!guest || !host || guest[0] != '/' || host[0] != '/') return 0;
    size_t gl = strlen(guest), hl = strlen(host);
    /* orez koncove '/' (krome koren "/") */
    while (gl > 1 && guest[gl - 1] == '/') gl--;
    while (hl > 1 && host[hl - 1] == '/') hl--;
    if (gl >= LX_BIND_GLEN || hl >= LX_BIND_HLEN) return 0;
    int slot = -1;
    for (int i = 0; i < g_binds_n; i++) {
        if (g_binds[i].active && g_binds[i].glen == (unsigned short)gl &&
            strncmp(g_binds[i].guest, guest, gl) == 0) { slot = i; break; }
        if (!g_binds[i].active && slot < 0) slot = i;
    }
    if (slot < 0) {
        if (g_binds_n >= LX_BIND_MAX) return 0;
        slot = g_binds_n++;
    }
    lx_bind_t *b = &g_binds[slot];
    b->active = 1; b->ro = (unsigned char)(ro ? 1 : 0);
    b->glen = (unsigned short)gl; b->hlen = (unsigned short)hl;
    memcpy(b->guest, guest, gl); b->guest[gl] = 0;
    memcpy(b->host, host, hl);   b->host[hl] = 0;
    return 1;
}

/* Vychozi identity bindy = dnesni exclude list (host==guest, nechat). */
static void binds_fill_defaults(void) {
    static const char *excl[] = {
        "/proc", "/dev", "/sys", "/system", "/data", "/apex", "/linkerconfig",
        "/metadata", "/sdcard", "/storage", "/vendor", "/odm", "/product",
        "/persist", "/cache", "/config", "/debug_ramdisk", NULL
    };
    for (int i = 0; excl[i]; i++) bind_add(excl[i], excl[i], 0);
    /* /dev/shm -> $ROOTFS/dev/shm (delsi prefix nez identity /dev): nahrazuje
     * drivejsi special-case v shim_translate. */
    if (g_shim_root && g_shim_root[0]) {
        char h[LX_BIND_HLEN];
        int k = snprintf(h, sizeof h, "%s/dev/shm", g_shim_root);
        if (k > 0 && (size_t)k < sizeof h) bind_add("/dev/shm", h, 0);
    }
}

/* Naparsuje jeden spec `host:guest[:ro]` (proot -b styl). guest/host absolutni.
 * Prazdne/komentar (#) ignoruje. */
static void bind_parse_spec(const char *spec) {
    if (!spec || !spec[0] || spec[0] == '#') return;
    char buf[LX_BIND_GLEN + LX_BIND_HLEN + 16];
    size_t sl = strlen(spec);
    if (sl >= sizeof buf) return;
    memcpy(buf, spec, sl + 1);
    char *colon = strchr(buf, ':');
    if (!colon) return;
    *colon = 0;
    char *host = buf;
    char *guest = colon + 1;
    int ro = 0;
    char *c2 = strchr(guest, ':');
    if (c2) { *c2 = 0; if (strcmp(c2 + 1, "ro") == 0) ro = 1; }
    bind_add(guest, host, ro);
}

/* LX_BINDS=host:guest[,host:guest...] a soubor $ROOTFS/etc/lx-binds (radky). */
static void binds_load_config(void) {
    char path[LX_BIND_HLEN];
    if (g_shim_root && g_shim_root[0]) {
        int k = snprintf(path, sizeof path, "%s/etc/lx-binds", g_shim_root);
        if (k > 0 && (size_t)k < sizeof path) {
            FILE *f = fopen(path, "r");
            if (f) {
                char line[LX_BIND_GLEN + LX_BIND_HLEN + 16];
                while (fgets(line, sizeof line, f)) {
                    size_t l = strlen(line);
                    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' ||
                                 line[l - 1] == ' ' || line[l - 1] == '\t')) line[--l] = 0;
                    char *s = line; while (*s == ' ' || *s == '\t') s++;
                    bind_parse_spec(s);
                }
                fclose(f);
            }
        }
    }
    const char *env = getenv("LX_BINDS");
    if (env && env[0]) {
        char buf[2048];
        size_t el = strlen(env);
        if (el < sizeof buf) {
            memcpy(buf, env, el + 1);
            char *tok = strtok(buf, ",");
            while (tok) { bind_parse_spec(tok); tok = strtok(NULL, ","); }
        }
    }
}

/* Volat jednou z run_ownall PO nastaveni g_shim_root (bionic TP). */
static void binds_init(void) {
    g_binds_n = 0;
    binds_fill_defaults();
    binds_load_config();
    g_binds_ready = 1;
    if (elf_debug()) {
        fprintf(stderr, "[binds] %d zaznamu:\n", g_binds_n);
        for (int i = 0; i < g_binds_n; i++)
            if (g_binds[i].active)
                fprintf(stderr, "  %s -> %s%s\n", g_binds[i].guest, g_binds[i].host,
                        g_binds[i].ro ? " (ro)" : "");
    }
}

#endif /* ELF_LOADER_BINDS_H */
