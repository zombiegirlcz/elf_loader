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
 * Proces s JINYM LX_BINDS (nebo ROOTFS) nez tabulka, kterou zdedil, se
 * nepripoji: jine LX_BINDS = soukroma kopie zdedene tabulky + nove bindy
 * (semantika `unshare -m` + mount; rodic zmenu nevidi), jiny ROOTFS = cista
 * nova tabulka. Bez toho vnoreny `LX_BINDS=... elf_loader` LX_BINDS tise
 * ignoroval (shim_child_envp LX_BINDS_FILE vnucuje).
 *
 * Uklid: kazdy proces drzi na souboru flock(LOCK_SH) (fd O_CLOEXEC, po
 * re-execu ho loader znovu otevre). Zakladani nove tabulky smaze soubory,
 * na kterych uz nikdo zamek nedrzi (LOCK_EX|LOCK_NB uspeje) -> soubory se
 * nehromadi (drive jeden ~50 KB soubor na kazde top-level spusteni).
 *
 * Vklada se do main.c. bind_kind/bind_translate/bind_reverse/bind_add/
 * bind_remove bezi na HORKE ceste pod GUEST TP -> jen shim_str* a zadne velke
 * buffery na zasobniku. binds_init/binds_load_config bezi pri startu pod
 * bionickym TP (libc OK).
 */
#ifndef ELF_LOADER_BINDS_H
#define ELF_LOADER_BINDS_H

#include <dirent.h>
#include <sys/file.h>
#include <time.h>

#define LX_BIND_MAX    64
#define LX_BIND_GLEN   256
#define LX_BIND_HLEN   512
#define LX_BIND_MAGIC  0x4c584232u   /* "LXB2" (v2: + root/cfg) */

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
    unsigned int root;         /* binds_hash(ROOTFS), pro ktery tabulka vznikla */
    unsigned int cfg;          /* binds_hash(LX_BINDS), se kterym vznikla */
    lx_bind_t e[LX_BIND_MAX];
} lx_bind_tab_t;

static lx_bind_tab_t *g_tab = NULL;   /* MAP_SHARED, nebo &g_tab_local fallback */
static lx_bind_tab_t g_tab_local;     /* fallback kdyz mmap selze (bez sdileni) */
static int g_binds_ready = 0;
static int g_binds_fd = -1;           /* drzi flock(LOCK_SH) = "tabulka v uzivani" */
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

/* 1 kdyz guest `p` JE mountpoint (presne guest prefix) aktivniho remap bindu.
 * Jadro rmdir/unlink/rename mountpointu odmita EBUSY; bez toho `rm -rf
 * <mountpoint>` po vyprazdneni smazal i samotny ZDROJ bindu. Identity bindy
 * (/proc, /dev, ...) neresime - ty prelozeni nemeni. */
static int bind_is_mountpoint(const char *p) {
    if (!g_binds_ready || !p || p[0] != '/') return 0;
    size_t pl = shim_strlen(p);
    while (pl > 1 && p[pl - 1] == '/') pl--;
    int i = bind_longest(p);
    if (i < 0) return 0;
    lx_bind_t *b = &g_tab->e[i];
    return (!bind_is_identity(b) && b->glen == pl) ? 1 : 0;
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

/* bind_reverse na miste (L1/L2 getcwd: buffer guesta, zadny druhy buffer na
 * cizim zasobniku). `buf` = host cesta s NUL, `cap` = kapacita. Vraci novou
 * delku BEZ NUL, nebo 0 (zadny remap bind / nevejde se -> nechat). */
static size_t bind_reverse_inplace(char *buf, size_t cap) {
    if (!g_binds_ready || !buf || buf[0] != '/') return 0;
    int best = -1, best_len = -1;
    unsigned int cnt = __atomic_load_n(&g_tab->count, __ATOMIC_ACQUIRE);
    for (unsigned int i = 0; i < cnt && i < LX_BIND_MAX; i++) {
        lx_bind_t *b = &g_tab->e[i];
        if (!b->active || bind_is_identity(b)) continue;
        int m = bind_prefix_match(buf, b->host, b->hlen);
        if (m > best_len) { best_len = m; best = (int)i; }
    }
    if (best < 0) return 0;
    lx_bind_t *b = &g_tab->e[best];
    size_t restlen = shim_strlen(buf + b->hlen);
    if ((size_t)b->glen + restlen + 1 > cap) return 0;
    binds_move(buf + b->glen, buf + b->hlen, restlen + 1);
    shim_memcpy(buf, b->guest, b->glen);
    return (size_t)b->glen + restlen;
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

/* $ROOTFS/etc/lx-binds (jako fstab). */
static void binds_load_file(void) {
    char path[LX_BIND_HLEN];
    if (!g_shim_root || !g_shim_root[0]) return;
    int k = snprintf(path, sizeof path, "%s/etc/lx-binds", g_shim_root);
    if (k <= 0 || (size_t)k >= sizeof path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
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

/* env LX_BINDS (proot -b styl, carkou oddelene). */
static void binds_load_env(void) {
    const char *env = getenv("LX_BINDS");
    if (!env || !env[0]) return;
    char buf[2048];
    size_t el = strlen(env);
    if (el >= sizeof buf) return;
    memcpy(buf, env, el + 1);
    char *tok = strtok(buf, ",");
    while (tok) { bind_parse_spec(tok); tok = strtok(NULL, ","); }
}

/* FNV-1a; 0 = prazdne/nenastavene, jinak nenulove. */
static unsigned int binds_hash(const char *s) {
    if (!s || !s[0]) return 0;
    unsigned int h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h | 1u;
}

/* Adresar a prefix souboru tabulek (stejne pro zakladani i uklid). */
static void binds_dir(char *dir, size_t n, const char **pfx) {
    const char *lx = getenv("LX_LOG");
    if (lx && lx[0]) { snprintf(dir, n, "%s", lx); *pfx = "binds."; }
    else if (g_shim_root && g_shim_root[0]) { snprintf(dir, n, "%s/tmp", g_shim_root); *pfx = ".lxbinds."; }
    else { snprintf(dir, n, "/tmp"); *pfx = ".lxbinds."; }
}

/* Smaze soubory tabulek, na kterych nikdo nedrzi flock(LOCK_SH). Cerstve
 * (< 5 s) preskakuje: zakladajici proces mezi open a flock zamek jeste nema. */
static void binds_gc(const char *dir, const char *pfx) {
    DIR *d = opendir(dir);
    if (!d) return;
    size_t pl = strlen(pfx);
    time_t now = time(NULL);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl <= pl + 4 || strncmp(de->d_name, pfx, pl) != 0 ||
            strcmp(de->d_name + nl - 4, ".tab") != 0) continue;
        char p[LX_BIND_HLEN];
        int k = snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (k <= 0 || (size_t)k >= sizeof p) continue;
        int fd = open(p, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && now - st.st_mtime >= 5 &&
            flock(fd, LOCK_EX | LOCK_NB) == 0)
            unlink(p);
        close(fd);
    }
    closedir(d);
}

/* Zalozi novy soubor tabulky (jmeno pid + ns -> zadna kolize s cizim zivym
 * souborem; O_EXCL, nikdy O_TRUNC cizi tabulky), drzi na nem LOCK_SH. */
static lx_bind_tab_t *binds_create(char *path, size_t n) {
    char dir[LX_BIND_HLEN];
    const char *pfx;
    binds_dir(dir, sizeof dir, &pfx);
    binds_gc(dir, pfx);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int k = snprintf(path, n, "%s/%s%d.%ld.tab", dir, pfx, (int)getpid(), (long)ts.tv_nsec);
    if (k <= 0 || (size_t)k >= n) return NULL;
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    flock(fd, LOCK_SH);
    if (ftruncate(fd, (off_t)sizeof(lx_bind_tab_t)) != 0) { close(fd); unlink(path); return NULL; }
    void *m = mmap(NULL, sizeof(lx_bind_tab_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); unlink(path); return NULL; }
    g_binds_fd = fd;
    return (lx_bind_tab_t *)m;
}

/* Namapuje sdilenou tabulku. Zdedena (LX_BINDS_FILE) se stejnym ROOTFS
 * i LX_BINDS -> pripojit. Jinak nova: top-level / jiny ROOTFS = defaults +
 * lx-binds + LX_BINDS; jen jine LX_BINDS = kopie zdedene + LX_BINDS. Cestu
 * zverejni potomkum. Fallback na per-proces static (bez sdileni). */
static void binds_init(void) {
    const char *existing = getenv("LX_BINDS_FILE");
    unsigned int root = binds_hash(g_shim_root), cfg = binds_hash(getenv("LX_BINDS"));
    lx_bind_tab_t *parent = NULL;
    int pfd = -1, attached = 0, forked = 0;
    char path[LX_BIND_HLEN];

    if (existing && existing[0]) {
        pfd = open(existing, O_RDWR | O_CLOEXEC);
        if (pfd >= 0) {
            void *m = mmap(NULL, sizeof(lx_bind_tab_t), PROT_READ | PROT_WRITE,
                           MAP_SHARED, pfd, 0);
            if (m != MAP_FAILED && ((lx_bind_tab_t *)m)->magic == LX_BIND_MAGIC &&
                ((lx_bind_tab_t *)m)->root == root)
                parent = (lx_bind_tab_t *)m;
            else if (m != MAP_FAILED)
                munmap(m, sizeof(lx_bind_tab_t));
        }
        if (parent && parent->cfg == cfg) {    /* tataz session -> pripojit */
            flock(pfd, LOCK_SH);
            g_binds_fd = pfd;
            g_tab = parent;
            snprintf(path, sizeof path, "%s", existing);
            attached = 1;
        }
    }

    if (!attached) {
        g_tab = binds_create(path, sizeof path);
        if (!g_tab) { g_tab = &g_tab_local; path[0] = 0; }
        if (parent) {                          /* unshare -m: zdedene mounty */
            memcpy(g_tab, parent, sizeof *g_tab);
            g_tab->cfg = cfg;
            g_binds_ready = 1;
            binds_load_env();                  /* lx-binds uz v rodici je */
            forked = 1;
        } else {
            memset(g_tab, 0, sizeof *g_tab);
            g_tab->magic = LX_BIND_MAGIC;
            g_tab->root = root;
            g_tab->cfg = cfg;
            g_binds_ready = 1;                 /* bind_add potrebuje g_tab */
            binds_fill_defaults();
            binds_load_file();
            binds_load_env();
        }
        if (parent) munmap(parent, sizeof(lx_bind_tab_t));
        if (pfd >= 0) close(pfd);
        if (path[0])
            setenv("LX_BINDS_FILE", path, 1);  /* deti zdedi a jen namapuji */
    }
    g_binds_ready = 1;
    if (path[0] && strlen(path) < sizeof g_binds_file)
        memcpy(g_binds_file, path, strlen(path) + 1);

    if (elf_debug()) {
        fprintf(stderr, "[binds] %s=%s gen=%u count=%u:\n",
                attached ? "attach" : forked ? "fork" : "create",
                path[0] ? path : "(lokalni)", g_tab->gen, g_tab->count);
        for (unsigned int i = 0; i < g_tab->count && i < LX_BIND_MAX; i++)
            if (g_tab->e[i].active)
                fprintf(stderr, "  %s -> %s%s\n", g_tab->e[i].guest, g_tab->e[i].host,
                        g_tab->e[i].ro ? " (ro)" : "");
    }
}

#endif /* ELF_LOADER_BINDS_H */
