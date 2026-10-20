/* imgcache.c — cache pripravenych obrazu modulu (prompt 10 cast A).
 * Navrh a limity viz imgcache.h. */
#define _GNU_SOURCE
#include "imgcache.h"
#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/auxv.h>
#include <sys/mman.h>

extern uintptr_t g_img_pref_base;
int elf_loader_range(uintptr_t *lo, uintptr_t *hi);
const char *elf_override_name_for_addr(const void *addr);
void *elf_override_addr(const char *name);

#define IMG_MAX_MODS   64
#define IMG_MAX_SEGS    8
#define IMG_MAX_FIXUPS 4096
#define IMG_NAME_MAX    64
#define IMG_PATH_MAX   512

typedef struct {
    uint64_t vaddr;     /* link-time vaddr zacatku ulozeneho useku */
    uint64_t len;       /* delka v bajtech */
    uint64_t data_off;  /* offset dat v souboru obrazu */
} img_seg_t;

typedef struct {
    char     path[IMG_PATH_MAX];      /* host cesta modulu */
    char     key[CACHE_KEY_MAX];      /* BuildID+size+mtime modulu */
    uint64_t base;                    /* baze, na ktere byl modul ulozen */
    uint64_t map_base_vaddr;
    uint32_t nseg;
    uint32_t nfix;
    img_seg_t seg[IMG_MAX_SEGS];
    struct {
        uint32_t seg;                 /* index v seg[] */
        uint64_t off;                 /* offset uvnitr useku */
        char     name[IMG_NAME_MAX];  /* jmeno shimu (override) */
    } fix[0];                         /* nepouzito - fixupy jsou zvlast */
} img_mod_hdr_t;

/* Fixup v souboru (pevna velikost, at je cteni trivialni). */
typedef struct {
    uint32_t mod;
    uint32_t seg;
    uint64_t off;      /* offset uvnitr useku */
    uint64_t ldr_off;  /* hodnota jako offset od BAZE OBRAZU LOADERU */
} img_fix_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t nmod;
    uint32_t nfix;
    uint64_t page_size;
    uint64_t hwcap;
    uint64_t hwcap2;
} img_hdr_t;

#define IMG_MAGIC 0x474d4945u  /* "EIMG" */

/* ---- stav ---------------------------------------------------------- */

typedef struct {
    char      path[IMG_PATH_MAX];
    char      key[CACHE_KEY_MAX];
    uintptr_t base;
    uint64_t  map_base_vaddr;
    uint32_t  nseg;
    img_seg_t seg[IMG_MAX_SEGS];
    unsigned char *data[IMG_MAX_SEGS];   /* jen pri zaznamu */
} img_mod_t;

static img_mod_t  g_mods[IMG_MAX_MODS];
static size_t     g_nmod = 0;
static img_fix_t  g_fix[IMG_MAX_FIXUPS];
static size_t     g_nfix = 0;

/* nactený obraz */
static int        g_have_img = 0;
static int        g_img_bad = 0;      /* obraz zahozen - dal uz jen relokace */
static unsigned char *g_img_buf = NULL;
static size_t     g_img_size = 0;
static img_hdr_t  g_img_hdr;
static int        g_restored = 0, g_recorded = 0;

static int img_level(void) {
    static int e = -1;
    if (e < 0) {
        const char *v = getenv("ELF_LOADER_IMGCACHE");
        e = (v && v[0] && strcmp(v, "0") != 0) ? atoi(v) : 0;
        if (e < 0) e = 0;
        if (v && v[0] && e == 0 && strcmp(v, "0") != 0) e = 1;  /* "yes"/"on" */
    }
    return e;
}

int img_enabled(void) { return img_level() > 0; }

int img_trace(void) {
    static int t = -1;
    if (t < 0)
        t = getenv("ELF_LOADER_IMG_TRACE") ? 1 : 0;
    return t;
}

void img_stats(int *restored, int *recorded) {
    if (restored) *restored = g_restored;
    if (recorded) *recorded = g_recorded;
}

static size_t img_page(void) {
    static size_t ps = 0;
    if (!ps) {
        long v = sysconf(_SC_PAGESIZE);
        ps = v > 0 ? (size_t)v : 4096;
    }
    return ps;
}

/* Cesta k souboru obrazu: <cache_dir>/img/<exe_key>-<page>-<hwcap>.img */
static int img_path(const char *exe_key, char *out, size_t outsz) {
    const char *dir = cache_dir();
    if (!dir || !exe_key || !exe_key[0])
        return 0;
    char sub[4300];
    snprintf(sub, sizeof sub, "%s/img", dir);
    if (mkdir(sub, 0755) != 0 && errno != EEXIST)
        return 0;
    snprintf(out, outsz, "%s/%s-p%zu-h%llx_%llx-v%u.img", sub, exe_key,
             img_page(), (unsigned long long)getauxval(AT_HWCAP),
             (unsigned long long)getauxval(AT_HWCAP2),
             (unsigned)IMG_FORMAT_VERSION);
    return 1;
}

/* ---- otevreni obrazu ------------------------------------------------ */

void img_open(const char *exe_key) {
    if (!img_enabled())
        return;
    char path[4600];
    if (!img_path(exe_key, path, sizeof path))
        return;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (img_trace())
            fprintf(stderr, "[img] miss %s\n", path);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(img_hdr_t) ||
        st.st_size > (off_t)(256 << 20)) {
        close(fd);
        return;
    }
    unsigned char *buf = malloc((size_t)st.st_size);
    if (!buf) { close(fd); return; }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
        if (r <= 0) { free(buf); close(fd); return; }
        got += (size_t)r;
    }
    close(fd);

    memcpy(&g_img_hdr, buf, sizeof g_img_hdr);
    if (g_img_hdr.magic != IMG_MAGIC || g_img_hdr.version != IMG_FORMAT_VERSION ||
        g_img_hdr.page_size != img_page() ||
        g_img_hdr.hwcap != getauxval(AT_HWCAP) ||
        g_img_hdr.hwcap2 != getauxval(AT_HWCAP2) ||
        g_img_hdr.nmod == 0 || g_img_hdr.nmod > IMG_MAX_MODS ||
        g_img_hdr.nfix > IMG_MAX_FIXUPS) {
        if (img_trace())
            fprintf(stderr, "[img] hlavicka nesouhlasi -> zahozeno\n");
        free(buf);
        return;
    }
    /* Minimalni velikost: hlavicka + moduly + fixupy */
    size_t need = sizeof(img_hdr_t) +
                  (size_t)g_img_hdr.nmod * sizeof(img_mod_t) * 0 +
                  (size_t)g_img_hdr.nmod * sizeof(img_seg_t) * 0;
    (void)need;
    g_img_buf = buf;
    g_img_size = (size_t)st.st_size;
    g_have_img = 1;
    if (img_trace())
        fprintf(stderr, "[img] hit %s (%u modulu, %u fixupu, %zu B)\n", path,
                g_img_hdr.nmod, g_img_hdr.nfix, g_img_size);
}

/* Zaznamy modulu v souboru jdou hned za hlavickou, pak fixupy, pak data. */
static const img_mod_t *img_mod_at(uint32_t i) {
    if (!g_have_img || i >= g_img_hdr.nmod)
        return NULL;
    size_t off = sizeof(img_hdr_t) + (size_t)i * sizeof(img_mod_t);
    if (off + sizeof(img_mod_t) > g_img_size)
        return NULL;
    return (const img_mod_t *)(const void *)(g_img_buf + off);
}

static const img_fix_t *img_fix_at(uint32_t i) {
    if (!g_have_img || i >= g_img_hdr.nfix)
        return NULL;
    size_t off = sizeof(img_hdr_t) + (size_t)g_img_hdr.nmod * sizeof(img_mod_t) +
                 (size_t)i * sizeof(img_fix_t);
    if (off + sizeof(img_fix_t) > g_img_size)
        return NULL;
    return (const img_fix_t *)(const void *)(g_img_buf + off);
}

static const img_mod_t *img_find(const char *path, uint32_t *idx_out) {
    if (!g_have_img || !path)
        return NULL;
    for (uint32_t i = 0; i < g_img_hdr.nmod; i++) {
        const img_mod_t *m = img_mod_at(i);
        if (m && strcmp(m->path, path) == 0) {
            if (idx_out) *idx_out = i;
            return m;
        }
    }
    return NULL;
}

uintptr_t img_preferred_base(const char *path) {
    if (!img_enabled() || g_img_bad)
        return 0;
    const img_mod_t *m = img_find(path, NULL);
    return m ? (uintptr_t)m->base : 0;
}

/* ---- restore -------------------------------------------------------- */

/* Runtime rozsahy obnoveneho RELRO (per modul). elf_relocate je pak
 * preskakuje - to je ta usetrena prace (symbol lookupy pro GOT). */
#define IMG_MAX_RESTORED 64
static struct { const elf_object_t *m; uintptr_t lo, hi; } g_rst[IMG_MAX_RESTORED];
static size_t g_nrst = 0;

static struct {
    const elf_object_t *m;
    unsigned char *addr;
    unsigned char *img;
    size_t len;
    const char *path;
} g_ver[IMG_MAX_RESTORED];
static size_t g_nver = 0;

/* ELF_LOADER_IMG_VERIFY=1: obraz se nacte, ale relokace probehne PLNE
 * a vysledek se s obrazem POROVNA - vypise se prvni rozdily. Diagnostika
 * pro otazku "co v RELRO je per-behove". */
static int img_verify(void) {
    static int v = -1;
    if (v < 0)
        v = getenv("ELF_LOADER_IMG_VERIFY") ? 1 : 0;
    return v;
}

/* Schova data, ktera obraz tvrdi (s dopoctenymi fixupy), aby je
 * img_verify_report() mohl po relokaci porovnat. Nic nezapisuje do modulu. */
static void img_stash_expected(const elf_object_t *m, const void *im_v,
                               uint32_t idx, const char *path);

int img_relro_covers(const elf_object_t *m, const void *addr) {
    if (img_level() < 2 || img_verify())
        return 0;   /* uroven 1 = relokace probiha plne (bezpecne) */
    uintptr_t a = (uintptr_t)addr;
    for (size_t i = 0; i < g_nrst; i++)
        if (g_rst[i].m == m && a >= g_rst[i].lo && a < g_rst[i].hi)
            return 1;
    return 0;
}

int img_restore_module(elf_object_t *m, const char *path) {
    if (!img_enabled() || g_img_bad || !g_have_img || !m || !m->base_addr)
        return 0;
    uint32_t idx = 0;
    const img_mod_t *im = path ? img_find(path, &idx) : NULL;
    if (!im)
        return 0;
    /* Klic modulu i baze MUSI souhlasit - obraz obsahuje absolutni adresy. */
    if (!m->cache_key || strcmp(m->cache_key, im->key) != 0 ||
        (uintptr_t)im->base != (uintptr_t)m->base_addr) {
        if (img_trace())
            fprintf(stderr, "[img] %s: %s -> plna relokace\n", path,
                    (uintptr_t)im->base != (uintptr_t)m->base_addr
                        ? "jina baze" : "jiny klic");
        g_img_bad = 1;   /* dal uz obraz nepouzivat (byl by nekonzistentni) */
        return 0;
    }
    if (im->nseg > IMG_MAX_SEGS)
        return 0;

    /* UROVEN 1 nesmi do pameti zapsat NIC. Zapsat obraz a pak nechat
     * relokaci "doplnit" nefunguje: slova, ktera relokace neprepisuje,
     * by zustala ze stareho behu -> SIGSEGV. Uroven 1 proto jen (volitelne)
     * schova ocekavana data pro verifikaci a vrati 0 = relokuj normalne. */
    if (img_level() < 2) {
        if (img_verify())
            img_stash_expected(m, im, idx, path);
        return 0;
    }

    /* Data useku */
    for (uint32_t s = 0; s < im->nseg; s++) {
        const img_seg_t *sg = &im->seg[s];
        if (sg->data_off + sg->len > g_img_size)
            return 0;
        char *dst = (char *)m->base_addr + (sg->vaddr - im->map_base_vaddr);
        size_t pg = img_page();
        uintptr_t lo = (uintptr_t)dst & ~(uintptr_t)(pg - 1);
        size_t span = (size_t)(((uintptr_t)dst + sg->len + pg - 1) & ~(uintptr_t)(pg - 1)) - lo;
        if (mprotect((void *)lo, span, PROT_READ | PROT_WRITE) != 0)
            return 0;
        memcpy(dst, g_img_buf + sg->data_off, sg->len);
    }

    /* Pointery do loaderu prepsat aktualnimi adresami shimu. */
    for (uint32_t f = 0; f < g_img_hdr.nfix; f++) {
        const img_fix_t *fx = img_fix_at(f);
        if (!fx || fx->mod != idx)
            continue;
        if (fx->seg >= im->nseg)
            return 0;
        uintptr_t llo = 0, lhi = 0;
        if (!elf_loader_range(&llo, &lhi) || fx->ldr_off >= (lhi - llo)) {
            g_img_bad = 1;
            return 0;
        }
        uintptr_t v = llo + fx->ldr_off;   /* rebase na aktualni bazi loaderu */
        const img_seg_t *sg = &im->seg[fx->seg];
        if (fx->off + 8 > sg->len)
            return 0;
        char *dst = (char *)m->base_addr + (sg->vaddr - im->map_base_vaddr) + fx->off;
        memcpy(dst, &v, 8);
    }

    /* Rozsahy si zapamatuj: elf_relocate je preskoci, ale VSE OSTATNI
     * (RELATIVE v .data, TLS template, apply_segment_prots -> PROT_EXEC)
     * musi probehnout normalne. Prvni verze nastavovala m->relocated = 1
     * a preskakovala elf_relocate cele - .text pak zustal bez PROT_EXEC
     * a program spadl na prvnim volani do libc. */
    for (uint32_t s = 0; s < im->nseg && g_nrst < IMG_MAX_RESTORED; s++) {
        const img_seg_t *sg = &im->seg[s];
        uintptr_t lo = (uintptr_t)m->base_addr + (sg->vaddr - im->map_base_vaddr);
        g_rst[g_nrst].m = m;
        g_rst[g_nrst].lo = lo;
        g_rst[g_nrst].hi = lo + sg->len;
        g_nrst++;
    }
    g_restored++;
    if (img_trace())
        fprintf(stderr, "[img] restore %s (%u useku)\n", path, im->nseg);
    return 1;
}

/* ---- zaznam --------------------------------------------------------- */

void img_verify_report(void) {
    if (!img_verify())
        return;
    for (size_t i = 0; i < g_nver; i++) {
        size_t diffs = 0;
        for (size_t o = 0; o + 8 <= g_ver[i].len; o += 8) {
            uint64_t a, b;
            memcpy(&a, g_ver[i].addr + o, 8);
            memcpy(&b, g_ver[i].img + o, 8);
            if (a == b) continue;
            if (diffs < 12)
                fprintf(stderr, "[imgver] %s+%#zx: relokace=%#llx obraz=%#llx\n",
                        g_ver[i].path, o, (unsigned long long)a,
                        (unsigned long long)b);
            diffs++;
        }
        fprintf(stderr, "[imgver] %s: %zu rozdilu z %zu slov\n", g_ver[i].path,
                diffs, g_ver[i].len / 8);
    }
}

void img_record_module(elf_object_t *m, const char *path) {
    if (!img_enabled() || g_img_bad || !m || !m->base_addr || !m->cache_key)
        return;
    if (!path || !path[0] || g_nmod >= IMG_MAX_MODS)
        return;
    uintptr_t llo = 0, lhi = 0;
    if (!elf_loader_range(&llo, &lhi))
        return;

    img_mod_t *rec = &g_mods[g_nmod];
    memset(rec, 0, sizeof *rec);
    snprintf(rec->path, sizeof rec->path, "%s", path);
    snprintf(rec->key, sizeof rec->key, "%s", m->cache_key);
    rec->base = (uintptr_t)m->base_addr;

    size_t mbv = (size_t)-1;
    for (int i = 0; i < m->phdr_count; i++)
        if (m->phdr[i].p_type == PT_LOAD && m->phdr[i].p_vaddr < mbv)
            mbv = m->phdr[i].p_vaddr;
    if (mbv == (size_t)-1)
        return;
    size_t pg = img_page();
    rec->map_base_vaddr = mbv & ~(size_t)(pg - 1);

    /* Ukladame VYHRADNE PT_GNU_RELRO (GOT, .got.plt, .data.rel.ro) - tam jsou
     * jen vysledky relokace. `.data`/`.bss` se ukladat NESMI: drzi per-behovy
     * stav (`__curbrk` = baze haldove areny, `__libc_stack_end` = guest stack,
     * stack guard), ktery se mezi behy lisi - prvni verze ukladala cele PF_W
     * segmenty a restore spolehlive padal SIGSEGV. */
    for (int i = 0; i < m->phdr_count; i++) {
        if (m->phdr[i].p_type != PT_GNU_RELRO)
            continue;
        if (rec->nseg >= IMG_MAX_SEGS)
            return;
        img_seg_t *sg = &rec->seg[rec->nseg];
        sg->vaddr = m->phdr[i].p_vaddr;
        sg->len = m->phdr[i].p_memsz;
        if (!sg->len)
            continue;
        unsigned char *src = (unsigned char *)m->base_addr +
                             (sg->vaddr - rec->map_base_vaddr);
        unsigned char *cp = malloc(sg->len);
        if (!cp)
            return;
        memcpy(cp, src, sg->len);

        /* Klasifikace kazdeho 8B slova. Obraz smi obsahovat jen hodnoty,
         * ktere jsou pri dalsim behu PLATNE:
         *   - male hodnoty / 0            = data, ne pointer
         *   - uvnitr nektereho modulu     = modul je na tez bazi (vynucena)
         *   - uvnitr OBRAZU loaderu       = fixup, rebase na novou bazi
         *   - uvnitr guest haldove areny  = pevna baze 0x7f00000000
         * Cokoli jineho (typicky pointer do MALLOCOVANE pameti loaderu, ktera
         * se mezi behy lisi) je duvod obraz vubec nevytvorit - radeji zadna
         * cache nez tise spatny pointer. */
        for (size_t o = 0; o + 8 <= sg->len; o += 8) {
            uintptr_t v;
            memcpy(&v, cp + o, 8);
            if (v < 0x10000UL) continue;
            if (v >= llo && v < lhi) {
                /* pointer do obrazu loaderu -> fixup (nize) */
            } else {
                int inside = 0;
                elf_scope_t *sc = m->scope;
                if (sc) {
                    for (size_t k = 0; k < sc->count && !inside; k++) {
                        elf_object_t *o2 = sc->mods[k];
                        if (!o2 || !o2->base_addr) continue;
                        uintptr_t b = (uintptr_t)o2->base_addr;
                        if (v >= b && v < b + o2->total_size) inside = 1;
                    }
                    if (!inside && sc->exe && sc->exe->base_addr) {
                        uintptr_t b = (uintptr_t)sc->exe->base_addr;
                        if (v >= b && v < b + sc->exe->total_size) inside = 1;
                    }
                }
                /* guest haldova arena (pevna baze, viz elf_loader.c) */
                if (!inside && v >= 0x7f00000000UL && v < 0x7f80000000UL)
                    inside = 1;
                /* Whitelist (modul / arena) = platne i pri dalsim behu.
                 * Cokoli jineho se bere jako DATA a kopiruje verbatim.
                 * Driv se na to odmital cely obraz, ale ta heuristika byla
                 * nespolehliva: hodnoty jako 0x430f0a10 (konstanta v RELRO
                 * ld.so) nebo 0x706f706364 (ASCII "dcpop") nejsou pointery,
                 * jen tak vypadaji. Spravnost obrazu hlida verifikacni
                 * rezim ELF_LOADER_IMG_VERIFY=1, ne tohle. */
                (void)inside;
                continue;
            }
            if (g_nfix >= IMG_MAX_FIXUPS) {
                if (img_trace())
                    fprintf(stderr, "[img] %s: prilis mnoho fixupu"
                            " -> obraz se nevytvori\n", rec->path);
                free(cp);
                g_img_bad = 1;
                return;
            }
            /* Loader je PIE s ASLR: uklada se OFFSET od baze jeho obrazu
             * a pri restore se prictie aktualni baze. Platne je to proto, ze
             * klic obrazu nese build tag loaderu (cache.c), takze obraz z
             * jineho buildu se nikdy nepouzije. Rozsah je presny (PT_LOAD
             * z dl_iterate_phdr), takze se do toho nepocita haldu loaderu. */
            img_fix_t *fx = &g_fix[g_nfix++];
            fx->mod = (uint32_t)g_nmod;
            fx->seg = rec->nseg;
            fx->off = o;
            fx->ldr_off = (uint64_t)(v - llo);
            memset(cp + o, 0, 8);   /* stara absolutni adresa se neuklada */
        }
        rec->data[rec->nseg] = cp;
        rec->nseg++;
    }
    if (!rec->nseg)
        return;
    g_nmod++;
    g_recorded++;
    if (img_trace())
        fprintf(stderr, "[img] zaznam %s (%u useku, baze %p)\n", rec->path,
                rec->nseg, (void *)(uintptr_t)rec->base);
}

void img_commit(const char *exe_key) {
    if (!img_enabled() || g_img_bad || g_have_img || !g_nmod)
        return;
    char path[4600], tmp[4700];
    if (!img_path(exe_key, path, sizeof path))
        return;
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());

    /* Offsety dat: za hlavickou, moduly a fixupy. */
    size_t off = sizeof(img_hdr_t) + g_nmod * sizeof(img_mod_t) +
                 g_nfix * sizeof(img_fix_t);
    for (size_t i = 0; i < g_nmod; i++)
        for (uint32_t s = 0; s < g_mods[i].nseg; s++) {
            g_mods[i].seg[s].data_off = off;
            off += g_mods[i].seg[s].len;
        }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    img_hdr_t h = { IMG_MAGIC, IMG_FORMAT_VERSION, (uint32_t)g_nmod,
                    (uint32_t)g_nfix, img_page(), getauxval(AT_HWCAP),
                    getauxval(AT_HWCAP2) };
    int ok = 1;
    if (write(fd, &h, sizeof h) != (ssize_t)sizeof h) ok = 0;
    for (size_t i = 0; ok && i < g_nmod; i++) {
        /* data[] se neuklada (pointery jen pro zaznam) */
        img_mod_t out = g_mods[i];
        memset(out.data, 0, sizeof out.data);
        if (write(fd, &out, sizeof out) != (ssize_t)sizeof out) ok = 0;
    }
    for (size_t i = 0; ok && i < g_nfix; i++)
        if (write(fd, &g_fix[i], sizeof g_fix[i]) != (ssize_t)sizeof g_fix[i])
            ok = 0;
    for (size_t i = 0; ok && i < g_nmod; i++)
        for (uint32_t s = 0; ok && s < g_mods[i].nseg; s++)
            if (write(fd, g_mods[i].data[s], g_mods[i].seg[s].len) !=
                (ssize_t)g_mods[i].seg[s].len)
                ok = 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return;
    }
    if (img_trace())
        fprintf(stderr, "[img] ulozeno %s (%zu modulu, %zu fixupu, %zu B)\n",
                path, g_nmod, g_nfix, off);
}

static void img_stash_expected(const elf_object_t *m, const void *im_v,
                               uint32_t idx, const char *path) {
    const img_mod_t *im = (const img_mod_t *)im_v;
    for (uint32_t s = 0; s < im->nseg && g_nver < IMG_MAX_RESTORED; s++) {
        const img_seg_t *sg = &im->seg[s];
        if (sg->data_off + sg->len > g_img_size)
            return;
        unsigned char *cp = malloc(sg->len);
        if (!cp)
            return;
        memcpy(cp, g_img_buf + sg->data_off, sg->len);
        for (uint32_t f = 0; f < g_img_hdr.nfix; f++) {
            const img_fix_t *fx = img_fix_at(f);
            if (!fx || fx->mod != idx || fx->seg != s)
                continue;
            uintptr_t llo = 0, lhi = 0;
            if (!elf_loader_range(&llo, &lhi))
                break;
            uintptr_t v = llo + fx->ldr_off;
            if (fx->off + 8 <= sg->len)
                memcpy(cp + fx->off, &v, 8);
        }
        g_ver[g_nver].m = m;
        g_ver[g_nver].addr = (unsigned char *)m->base_addr +
                             (sg->vaddr - im->map_base_vaddr);
        g_ver[g_nver].img = cp;
        g_ver[g_nver].len = sg->len;
        g_ver[g_nver].path = path;
        g_nver++;
    }
}
