#define _GNU_SOURCE
#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <elf.h>

/* ------------------------------------------------------------------ */
/* BuildID extraction (PT_NOTE / NT_GNU_BUILD_ID)                      */
/* ------------------------------------------------------------------ */

/* Najde v obrazu v pameti GNU BuildID. Vraci 1 a vyplni out/outlen. */
static int find_build_id(const void *file_map, size_t fsize,
                         unsigned char *out, size_t outsz, size_t *outlen)
{
    if (fsize < sizeof(Elf64_Ehdr))
        return 0;
    const unsigned char *m = (const unsigned char *)file_map;
    if (memcmp(m, ELFMAG, SELFMAG) != 0 || m[EI_CLASS] != ELFCLASS64)
        return 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file_map;
    if (eh->e_phoff == 0 || eh->e_phnum == 0 ||
        eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf64_Phdr) > fsize)
        return 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(m + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_NOTE)
            continue;
        if (ph[i].p_offset >= fsize)
            continue;
        const unsigned char *p = m + ph[i].p_offset;
        size_t end = ph[i].p_offset + ph[i].p_filesz;
        if (end > fsize)
            end = fsize;
        while (p + 12 <= m + end) {
            uint32_t nsz, dsz, typ;
            memcpy(&nsz, p, 4);
            memcpy(&dsz, p + 4, 4);
            memcpy(&typ, p + 8, 4);
            const unsigned char *name = p + 12;
            size_t namepad = (nsz + 3u) & ~3u;
            size_t descpad = (dsz + 3u) & ~3u;
            if (name + namepad > m + end)
                break;
            /* NT_GNU_BUILD_ID = 3, name "GNU\0" */
            if (typ == 3 && nsz >= 3 && name[0] == 'G' && name[1] == 'N' &&
                name[2] == 'U') {
                const unsigned char *desc = name + namepad;
                if (desc + descpad > m + end)
                    break;
                size_t n = dsz < outsz ? dsz : outsz;
                memcpy(out, desc, n);
                *outlen = n;
                return 1;
            }
            p = name + namepad + descpad;
        }
    }
    return 0;
}

static void hex(const unsigned char *b, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 0xf];
    }
    out[2 * n] = 0;
}

void cache_build_key(const void *file_map, size_t fsize, long long mtime_ns,
                     char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    out[0] = 0;
    char bid[2 * 32 + 1];
    unsigned char raw[32];
    size_t rawlen = 0;
    if (find_build_id(file_map, fsize, raw, sizeof raw, &rawlen) && rawlen > 0)
        hex(raw, rawlen, bid);
    else
        strcpy(bid, "nobid");
    snprintf(out, outsz, "%s_%llu_%lld_v%u", bid,
             (unsigned long long)fsize, mtime_ns, (unsigned)CACHE_FORMAT_VERSION);
}

/* ------------------------------------------------------------------ */
/* Cache directory                                                     */
/* ------------------------------------------------------------------ */

const char *cache_dir(void)
{
    static char dir[4096];
    static int resolved = 0;
    if (resolved)
        return dir[0] ? dir : NULL;
    resolved = 1;

    const char *lxlog = getenv("LX_LOG");
    const char *rootfs = getenv("ROOTFS");
    if (!rootfs || !rootfs[0])
        rootfs = getenv("ELF_ROOTFS");

    if (lxlog && lxlog[0])
        snprintf(dir, sizeof dir, "%s/cache", lxlog);
    else if (rootfs && rootfs[0])
        snprintf(dir, sizeof dir, "%s/root/elf_loader/cache", rootfs);
    else
        snprintf(dir, sizeof dir, "/tmp/elf_loader-cache");

    /* Vytvor adresar (jen finalni uroven; rodicovsky musi existovat).
     * Kdyz se nepodari, cache bude jen miss - exec to nezastavi. */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        if (cache_trace())
            fprintf(stderr, "[cache] mkdir(%s): %s\n", dir, strerror(errno));
    }
    return dir;
}

int cache_trace(void)
{
    static int t = -1;
    if (t < 0)
        t = getenv("ELF_LOADER_CACHE_TRACE") ? 1 : 0;
    return t;
}

/* ------------------------------------------------------------------ */
/* Serialization                                                       */
/* ------------------------------------------------------------------ */

#define CACHE_MAGIC 0x53434c45u /* "ELCS" little-endian */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n;
    uint32_t reserved;
} cache_hdr_t;

/* Odhad velikosti bufferu + zapis. */
static unsigned char *serialize(const cache_module_t *cm, size_t *out_sz)
{
    size_t need = sizeof(cache_hdr_t);
    for (int i = 0; i < CACHE_NR_COUNT; i++)
        need += 16 + (size_t)cm->list[i].n * 4;
    unsigned char *buf = malloc(need);
    if (!buf)
        return NULL;
    cache_hdr_t h = { CACHE_MAGIC, CACHE_FORMAT_VERSION, CACHE_NR_COUNT, 0 };
    unsigned char *p = buf;
    memcpy(p, &h, sizeof h); p += sizeof h;
    for (int i = 0; i < CACHE_NR_COUNT; i++) {
        int64_t nr = cm->nr[i];
        uint32_t cnt = cm->list[i].n;
        uint32_t pad = 0;
        memcpy(p, &nr, 8); p += 8;
        memcpy(p, &cnt, 4); p += 4;
        memcpy(p, &pad, 4); p += 4;
        if (cnt) {
            memcpy(p, cm->list[i].off, (size_t)cnt * 4);
            p += (size_t)cnt * 4;
        }
    }
    *out_sz = (size_t)(p - buf);
    return buf;
}

cache_module_t *cache_load_module(const char *key)
{
    if (!key || !key[0])
        return NULL;
    const char *dir = cache_dir();
    if (!dir)
        return NULL;
    char path[4600];
    snprintf(path, sizeof path, "%s/%s", dir, key);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(cache_hdr_t) ||
        st.st_size > (off_t)(1 << 20)) {   /* rozumny limit */
        close(fd);
        return NULL;
    }
    size_t sz = (size_t)st.st_size;
    unsigned char *buf = malloc(sz);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r <= 0) { free(buf); close(fd); return NULL; }
        got += (size_t)r;
    }
    close(fd);

    cache_hdr_t h;
    memcpy(&h, buf, sizeof h);
    if (h.magic != CACHE_MAGIC || h.version != CACHE_FORMAT_VERSION ||
        h.n != CACHE_NR_COUNT) {
        free(buf);
        return NULL;
    }
    const unsigned char *p = buf + sizeof h;
    const unsigned char *end = buf + sz;
    cache_module_t *cm = calloc(1, sizeof *cm);
    if (!cm) { free(buf); return NULL; }
    int ok = 1;
    for (int i = 0; i < CACHE_NR_COUNT && ok; i++) {
        if (p + 16 > end) { ok = 0; break; }
        int64_t nr; uint32_t cnt;
        memcpy(&nr, p, 8); p += 8;
        memcpy(&cnt, p, 4); p += 4;
        p += 4;
        if (cnt > CACHE_MAX_SITES || p + (size_t)cnt * 4 > end) { ok = 0; break; }
        cm->nr[i] = (long)nr;
        cm->list[i].n = cnt;
        if (cnt) {
            cm->list[i].off = malloc((size_t)cnt * 4);
            if (!cm->list[i].off) { ok = 0; break; }
            memcpy(cm->list[i].off, p, (size_t)cnt * 4);
            p += (size_t)cnt * 4;
        }
    }
    free(buf);
    if (!ok) { cache_module_free(cm); return NULL; }
    if (cache_trace())
        fprintf(stderr, "[cache] hit %s\n", key);
    return cm;
}

int cache_store_module(const char *key, const cache_module_t *cm)
{
    if (!key || !key[0] || !cm)
        return 0;
    const char *dir = cache_dir();
    if (!dir)
        return 0;
    size_t sz = 0;
    unsigned char *buf = serialize(cm, &sz);
    if (!buf)
        return 0;

    char tmp[4600], final[4600];
    snprintf(tmp, sizeof tmp, "%s/%s.tmp.%d", dir, key, (int)getpid());
    snprintf(final, sizeof final, "%s/%s", dir, key);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { free(buf); return 0; }
    size_t put = 0;
    int ok = 1;
    while (put < sz) {
        ssize_t w = write(fd, buf + put, sz - put);
        if (w <= 0) { ok = 0; break; }
        put += (size_t)w;
    }
    close(fd);
    free(buf);
    if (!ok) { unlink(tmp); return 0; }
    if (rename(tmp, final) != 0) { unlink(tmp); return 0; }
    if (cache_trace())
        fprintf(stderr, "[cache] store %s (%zu B)\n", key, sz);
    return 1;
}

void cache_module_free(cache_module_t *cm)
{
    if (!cm)
        return;
    for (int i = 0; i < CACHE_NR_COUNT; i++) {
        free(cm->list[i].off);
        cm->list[i].off = NULL;
    }
    free(cm);
}