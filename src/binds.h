/* src/binds.h — prompt 06: uzivatelska tabulka mountu / bindu.
 *
 * Nahrazuje natvrdo zapsany shim_excluded() longest-prefix tabulkou
 *   guest_prefix -> host_target [ro].
 * Vychozi obsah = dnesni exclude list jako IDENTITY bindy (guest==host), takze
 * default chovani je bajtove shodne. Navic pohlcuje /dev/shm special-case
 * (delsi prefix -> remap na $ROOTFS/dev/shm). Uzivatelske bindy prichazeji
 * z $ROOTFS/etc/lx-binds, env LX_BINDS (format `host:guest[:ro]` jako proot -b,
 * host = REALNA host cesta) a za behu z shim_mount (MS_BIND; tam je source
 * GUEST cesta a prelozi se).
 *
 * Cross-process: tabulka je MAP_SHARED soubor. Top-level loader ho zalozi a
 * cestu preda potomkum pres env LX_BINDS_FILE (deti ho jen namapuji). Runtime
 * shim_mount/umount zapisuje do sdilene mmapy -> potomci i sourozenci zmenu
 * vidi okamzite (count/active atomicky s release ordering). generacni citac
 * (gen) se bumpuje pri kazde zmene (diagnostika / budouci cache).
 *
 * Vklada se do main.c. bind_kind/bind_translate/bind_reverse/bind_add/
 * bind_remove bezi na HORKE ceste pod GUEST TP -> jen shim_str* a zadne velke
 * buffery na zasobniku. binds_init/binds_load_config bezi pri startu pod
 * bionickym TP (libc OK).
 */
#ifndef ELF_LOADER_BINDS_H
#define ELF_LOADER_BINDS_H

#define LX_BIND_MAX    64
#define LX_BIND_GLEN   256
#define LX_BIND_HLEN   512
#define LX_BIND_MAGIC  0x4c58424du   /* "LXBM" */

#ifndef MS_BIND
#define MS_BIND 0x1000
#endif
#ifndef MS_REC
#define MS_REC  0x4000
#endif

typedef struct {
    unsigned char active;      /* 0 = volny slot (umount) */
    unsigned char ro;          /* 1 = read-only (zapis -> EROFS) */
    unsigned short glen, hlen; /* delky bez NUL */
    char guest[LX_BIND_GLEN];
    char host[LX_BIND_HLEN];
} lx_bind_t;

typedef struct {
    unsigned int magic;
    unsigned int gen;          /* generacni citac (bump pri zmene) */
    unsigned int count;        /* high-water pocet slotu (vc. neaktivnich) */
    lx_bind_t e[LX_BIND_MAX];
} lx_bind_tab_t;

static lx_bind_tab_t *g_tab = NULL;   /* MAP_SHARED, nebo &g_tab_local fallback */
static lx_bind_tab_t g_tab_local;     /* fallback kdyz mmap selze (bez sdileni) */
static int g_binds_ready = 0;
/* Cesta ke sdilenemu souboru tabulky. Force-injektuje se do envp potomku
 * (LX_BINDS_FILE), aby jejich binds_init namapoval TUTEZ tabulku -> runtime
 * mount je cross-process. setenv v loaderu (bionic) do glibc environ guesta
 * nedosahne, proto propagace pres shim_child_envp jako u ROOTFS. */
static char g_binds_file[LX_BIND_HLEN] = {0};

/* bind_kind vysledky */
#define BIND_NONE     0   /* zadny bind -> default $ROOTFS prefix */
#define BIND_IDENTITY 1   /* host==guest (dnesni "excluded") -> nechat */
#define BIND_REMAP    2   /* host!=guest -> out naplneno remapem */

/* ── lookup (horka cesta, guest TP, jen shim_str*) ───────────────────── */

/* Komponentova shoda prefixu: `p` zacina `pref` a konci na hranici. pref=="/"
 * matchuje vse. Vraci delku shody (glen) nebo -1. */
static int bind_prefix_match(const char *p, const char *pref, int glen) {
    if (glen == 1 && pref[0] == '/') return (p[0] == '/') ? 1 : -1;
    if (shim_strncmp(p, pref, (size_t)glen) != 0) return -1;
    if (p[glen] == '/' || p[glen] == 0) return glen;
    return -1;
}

static int bind_is_identity(const lx_bind_t *b) {
    return b->glen == b->hlen && shim_strncmp(b->guest, b->host, b->glen) == 0;
}

/* Najde nejdelsi matchujici aktivni bind pro guest `p`. Vraci index nebo -1. */
static int bind_longest(const char *p) {
    int best = -1, best_len = -1;
    unsigned int n = __atomic_load_n(&g_tab->count, __ATOMIC_ACQUIRE);
    for (unsigned int i = 0; i < n && i < LX_BIND_MAX; i++) {
        if (!g_tab->e[i].active) continue;
        int m = bind_prefix_match(p, g_tab->e[i].guest, g_tab->e[i].glen);
        if (m > best_len) { best_len = m; best = (int)i; }
    }
    return best;
}

/* Jen druh shody, bez zapisu out (pro shim_excluded callery). */
static int bind_kind(const char *p) {
    if (!g_binds_ready) return BIND_NONE;
    int i = bind_longest(p);
    if (i < 0) return BIND_NONE;
    return bind_is_identity(&g_tab->e[i]) ? BIND_IDENTITY : BIND_REMAP;
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
    lx_bind_t *b = &g_tab->e[i];
    if (bind_is_identity(b)) return BIND_IDENTITY;
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

/* 1 kdyz guest `p` spada pod aktivni ro bind (zapisujici operace -> EROFS). */
static int bind_is_ro(const char *p) {
    if (!g_binds_ready) return 0;
    int i = bind_longest(p);
    return (i >= 0 && g_tab->e[i].ro) ? 1 : 0;
}

/* Zpetne mapovani host->guest (getcwd/readlink): host pod nejakym remap
 * host_targetem -> guest_prefix + zbytek. Vraci 1 a naplni out, jinak 0.
 * Default $ROOTFS->/ resi shim_strip_root (identity bindy se neresi). */
static int bind_reverse(const char *host, char *out, size_t n) {
    if (!g_binds_ready || !host || host[0] != '/') return 0;
    int best = -1, best_len = -1;
    unsigned int cnt = __atomic_load_n(&g_tab->count, __ATOMIC_ACQUIRE);
    for (unsigned int i = 0; i < cnt && i < LX_BIND_MAX; i++) {
        lx_bind_t *b = &g_tab->e[i];
        if (!b->active || bind_is_identity(b)) continue;
        int m = bind_prefix_match(host, b->host, b->hlen);
        if (m > best_len) { best_len = m; best = (int)i; }
    }
    if (best < 0) return 0;
    lx_bind_t *b = &g_tab->e[best];
    const char *rest = host + b->hlen;
    size_t restlen = shim_strlen(rest);
    if ((size_t)b->glen + restlen + 1 > n) return 0;
    shim_memcpy(out, b->guest, b->glen);
    shim_memcpy(out + b->glen, rest, restlen + 1);
    return 1;
}

/* ── mutace tabulky (guest TP safe: jen shim_str*, zadny malloc) ──────── */

/* Prida/aktualizuje bind guest->host[:ro]. Vraci 1 ok. Deduplikuje podle guest.
 * Zapisuje do sdilene mmapy; count/active atomicky s release orderingem, aby
 * soubezny ctenar v jinem procesu videl bud stary, nebo kompletni novy stav. */
static int bind_add(const char *guest, const char *host, int ro) {
    if (!g_tab || !guest || !host || guest[0] != '/' || host[0] != '/') return 0;
    size_t gl = shim_strlen(guest), hl = shim_strlen(host);
    while (gl > 1 && guest[gl - 1] == '/') gl--;
    while (hl > 1 && host[hl - 1] == '/') hl--;
    if (gl >= LX_BIND_GLEN || hl >= LX_BIND_HLEN) return 0;
    unsigned int cnt = g_tab->count;
    int slot = -1;
    for (unsigned int i = 0; i < cnt && i < LX_BIND_MAX; i++) {
        if (g_tab->e[i].active && g_tab->e[i].glen == (unsigned short)gl &&
            shim_strncmp(g_tab->e[i].guest, guest, gl) == 0) { slot = (int)i; break; }
        if (!g_tab->e[i].active && slot < 0) slot = (int)i;
    }
    int appended = 0;
    if (slot < 0) {
        if (cnt >= LX_BIND_MAX) return 0;
        slot = (int)cnt; appended = 1;
    }
    lx_bind_t *b = &g_tab->e[slot];
    b->active = 0;                             /* skryj behem zapisu */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    b->ro = (unsigned char)(ro ? 1 : 0);
    b->glen = (unsigned short)gl; b->hlen = (unsigned short)hl;
    shim_memcpy(b->guest, guest, gl); b->guest[gl] = 0;
    shim_memcpy(b->host, host, hl);   b->host[hl] = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    b->active = 1;
    if (appended) __atomic_store_n(&g_tab->count, cnt + 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_tab->gen, 1, __ATOMIC_RELEASE);
    return 1;
}

/* Odebere bind podle guest cesty (umount). Vraci 1 kdyz nasel. */
static int bind_remove(const char *guest) {
    if (!g_tab || !guest) return 0;
    size_t gl = shim_strlen(guest);
    while (gl > 1 && guest[gl - 1] == '/') gl--;
    unsigned int cnt = g_tab->count;
    for (unsigned int i = 0; i < cnt && i < LX_BIND_MAX; i++) {
        lx_bind_t *b = &g_tab->e[i];
        if (b->active && !bind_is_identity(b) && b->glen == (unsigned short)gl &&
            shim_strncmp(b->guest, guest, gl) == 0) {
            __atomic_store_n(&b->active, 0, __ATOMIC_RELEASE);
            __atomic_fetch_add(&g_tab->gen, 1, __ATOMIC_RELEASE);
            return 1;
        }
    }
    return 0;
}

/* ── naplneni (startup, bionic TP, libc OK) ──────────────────────────── */

static void binds_fill_defaults(void) {
    static const char *excl[] = {
        "/proc", "/dev", "/sys", "/system", "/data", "/apex", "/linkerconfig",
        "/metadata", "/sdcard", "/storage", "/vendor", "/odm", "/product",
        "/persist", "/cache", "/config", "/debug_ramdisk", NULL
    };
    for (int i = 0; excl[i]; i++) bind_add(excl[i], excl[i], 0);
    if (g_shim_root && g_shim_root[0]) {
        char h[LX_BIND_HLEN];
        int k = snprintf(h, sizeof h, "%s/dev/shm", g_shim_root);
        if (k > 0 && (size_t)k < sizeof h) bind_add("/dev/shm", h, 0);
    }
}

/* Naparsuje jeden spec `host:guest[:ro]` (proot -b styl, host = realna cesta). */
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

/* Namapuje sdilenou tabulku. Top-level (LX_BINDS_FILE neni v env) zalozi novy
 * soubor, naplni defaults+config a cestu zverejni potomkum; dite jen namapuje.
 * Fallback na per-proces static (bez sdileni) kdyz cokoli selze. */
static void binds_init(void) {
    const char *existing = getenv("LX_BINDS_FILE");
    int creating = !(existing && existing[0]);
    char path[LX_BIND_HLEN];
    int fd = -1;

    if (creating) {
        const char *dir = getenv("LX_LOG");
        if (dir && dir[0])
            snprintf(path, sizeof path, "%s/binds.%d.tab", dir, (int)getpid());
        else if (g_shim_root && g_shim_root[0])
            snprintf(path, sizeof path, "%s/tmp/.lxbinds.%d.tab", g_shim_root, (int)getpid());
        else
            snprintf(path, sizeof path, "/tmp/.lxbinds.%d.tab", (int)getpid());
        fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0 && ftruncate(fd, (off_t)sizeof(lx_bind_tab_t)) != 0) { close(fd); fd = -1; }
    } else {
        snprintf(path, sizeof path, "%s", existing);
        fd = open(path, O_RDWR);
    }

    if (fd >= 0) {
        void *m = mmap(NULL, sizeof(lx_bind_tab_t), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
        close(fd);
        if (m != MAP_FAILED) {
            g_tab = (lx_bind_tab_t *)m;
            size_t pl = strlen(path);          /* zapamatuj cestu pro propagaci */
            if (pl < sizeof g_binds_file) memcpy(g_binds_file, path, pl + 1);
        }
    }
    if (!g_tab) {                              /* fallback: nesdilena static */
        g_tab = &g_tab_local;
        creating = 1;
    }

    if (creating) {
        memset(g_tab, 0, sizeof *g_tab);
        g_tab->magic = LX_BIND_MAGIC;
        g_binds_ready = 1;                     /* bind_add potrebuje g_tab */
        binds_fill_defaults();
        binds_load_config();
        if (g_tab != &g_tab_local)
            setenv("LX_BINDS_FILE", path, 1);  /* deti zdedi a jen namapuji */
    } else {
        if (g_tab->magic != LX_BIND_MAGIC) {   /* poskozeny soubor -> defaults */
            memset(g_tab, 0, sizeof *g_tab);
            g_tab->magic = LX_BIND_MAGIC;
            g_binds_ready = 1;
            binds_fill_defaults();
            binds_load_config();
        } else {
            g_binds_ready = 1;
        }
    }

    if (elf_debug()) {
        fprintf(stderr, "[binds] %s=%s gen=%u count=%u:\n",
                creating ? "create" : "attach", path, g_tab->gen, g_tab->count);
        for (unsigned int i = 0; i < g_tab->count && i < LX_BIND_MAX; i++)
            if (g_tab->e[i].active)
                fprintf(stderr, "  %s -> %s%s\n", g_tab->e[i].guest, g_tab->e[i].host,
                        g_tab->e[i].ro ? " (ro)" : "");
    }
}

#endif /* ELF_LOADER_BINDS_H */
