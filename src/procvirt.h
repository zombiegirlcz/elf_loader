/* src/procvirt.h — prompt 07: virtualizace /proc pro guest.
 *
 * Guest nema videt loader ani host cesty:
 *   /proc/self/cmdline  -> guest argv (ne "elf_loader --ownall /data/...")
 *   /proc/self/maps     -> $ROOTFS/... jako guest cesty, mapovani loaderu
 *                          pod jmenem guest exe
 *   /proc/self/mounts, /proc/mounts, /etc/mtab, /proc/self/mountinfo
 *                       -> synteticky koren + tabulka bindu (prompt 06);
 *                          util-linux umount/findmnt i df je potrebuji
 * Plati i pro /proc/thread-self/... a /proc/<vlastni pid>/... open(at)
 * takove cesty JEN PRO CTENI vrati memfd se syntetickym obsahem (fallback:
 * soubor v $ROOTFS/tmp hned unlinknuty). Cokoli selze -> 0 a jde se na
 * skutecny soubor (nikdy hur nez dnes).
 *
 * Cizi procesy (ps cte /proc/<pid>/cmdline jineho guesta) tohle nepokryje -
 * na to run_ownall prepise kernelovou oblast argv (procvirt_rewrite_argarea).
 *
 * Bezi pod GUEST TP (jmenne hooky open/openat, L1 l1_before, L2 go_handle)
 * -> jen raw syscally a shim_str*, zadny snprintf/getpid/malloc. Pracovni
 * buffer je mmap (L1 muze bezet na 8 KB Go zasobniku, viz past 9).
 */
#ifndef ELF_LOADER_PROCVIRT_H
#define ELF_LOADER_PROCVIRT_H

#define PV_NONE      0
#define PV_CMDLINE   1
#define PV_MAPS      2
#define PV_MOUNTS    3
#define PV_MOUNTINFO 4
#define PV_CMDLINE_OTHER 5   /* /proc/<cizi pid>/cmdline: orez vycpavky */

#define PV_IN_CAP   (64 * 1024)
#define PV_OUT_CAP  (64 * 1024)

/* desitkovy zapis bez snprintf; vraci delku */
static int pv_utoa(char *d, unsigned long v) {
    char r[24]; int n = 0, k = 0;
    do { r[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) d[k++] = r[--n];
    d[k] = 0;
    return k;
}

/* "/proc/self/X", "/proc/thread-self/X", "/proc/<muj pid>/X" -> X, jinak NULL */
static const char *pv_self_rest(const char *p) {
    if (shim_strncmp(p, "/proc/", 6) != 0) return NULL;
    const char *q = p + 6;
    if (shim_strncmp(q, "self/", 5) == 0) return q + 5;
    if (shim_strncmp(q, "thread-self/", 12) == 0) return q + 12;
    if (q[0] < '0' || q[0] > '9') return NULL;
    char num[24];
    int nl = pv_utoa(num, (unsigned long)shim_raw_syscall6(172 /* getpid */, 0, 0, 0, 0, 0, 0));
    if (shim_strncmp(q, num, (size_t)nl) != 0 || q[nl] != '/') return NULL;
    return q + nl + 1;
}

static int pv_kind(const char *p) {
    if (!p || p[0] != '/') return PV_NONE;
    if (shim_strcmp(p, "/proc/mounts") == 0 || shim_strcmp(p, "/etc/mtab") == 0)
        return PV_MOUNTS;
    const char *r = pv_self_rest(p);
    if (!r) {
        /* cizi guest: run_ownall prepsal jeho kernelovou oblast argv, zbytek
         * je vycpany nulami a jadro je vrati -> "foo\0" + "3\0" + "\0\0..." */
        const char *q = p + 6;
        if (shim_strncmp(p, "/proc/", 6) != 0 || q[0] < '0' || q[0] > '9') return PV_NONE;
        while (*q >= '0' && *q <= '9') q++;
        return shim_strcmp(q, "/cmdline") == 0 ? PV_CMDLINE_OTHER : PV_NONE;
    }
    if (shim_strcmp(r, "cmdline") == 0) return PV_CMDLINE;
    if (shim_strcmp(r, "maps") == 0) return PV_MAPS;
    if (shim_strcmp(r, "mounts") == 0) return PV_MOUNTS;
    if (shim_strcmp(r, "mountinfo") == 0) return PV_MOUNTINFO;
    return PV_NONE;
}

typedef struct {
    long fd;
    char *out;
    size_t n;
    int err;
} pv_w_t;

static void pv_flush(pv_w_t *w) {
    size_t off = 0;
    while (!w->err && off < w->n) {
        long r = shim_raw_syscall6(64 /* write */, w->fd, (long)(w->out + off),
                                   (long)(w->n - off), 0, 0, 0);
        if (r <= 0) { w->err = 1; break; }
        off += (size_t)r;
    }
    w->n = 0;
}
static void pv_putn(pv_w_t *w, const char *s, size_t n) {
    while (n) {
        if (w->n == PV_OUT_CAP) pv_flush(w);
        size_t k = PV_OUT_CAP - w->n;
        if (k > n) k = n;
        shim_memcpy(w->out + w->n, s, k);
        w->n += k; s += k; n -= k;
    }
}
static void pv_puts(pv_w_t *w, const char *s) { pv_putn(w, s, shim_strlen(s)); }
static void pv_putc(pv_w_t *w, char c) { pv_putn(w, &c, 1); }
static void pv_putu(pv_w_t *w, unsigned long v) { char b[24]; pv_putn(w, b, (size_t)pv_utoa(b, v)); }
/* cesta v mounts/mountinfo: mezera, tab, \n a \ jako oktal (jako jadro) */
static void pv_put_esc(pv_w_t *w, const char *s) {
    for (; *s; s++) {
        char c = *s;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\\') {
            char e[4] = { '\\', (char)('0' + ((c >> 6) & 7)), (char)('0' + ((c >> 3) & 7)),
                          (char)('0' + (c & 7)) };
            pv_putn(w, e, 4);
        } else {
            pv_putc(w, c);
        }
    }
}

/* host cesta -> guest pohled: (rev) remap bind zpetne, pod $ROOTFS strip,
 * jinak beze zmeny. Vysledek do `out` (cap >= 4096). Zdroj bindu v mounts
 * se zpetne mapovat NESMI (ukazal by se jako vlastni mountpoint). */
static const char *pv_guestify(const char *host, char *out, size_t cap, int rev) {
    size_t hl = shim_strlen(host);
    if (hl + 1 > cap) return host;
    shim_memcpy(out, host, hl + 1);
    if (rev && bind_reverse_inplace(out, cap)) return out;
    size_t rl = (g_shim_root && g_shim_root[0]) ? shim_strlen(g_shim_root) : 0;
    if (rl && shim_strncmp(out, g_shim_root, rl) == 0 && (out[rl] == '/' || out[rl] == 0)) {
        if (!out[rl]) { out[0] = '/'; out[1] = 0; }
        else { size_t k = 0; for (size_t i = rl; ; i++) { out[k++] = out[i]; if (!out[i]) break; } }
    }
    return out;
}

/* Cizi cmdline s vycpavkou (vic nez jedna koncova \0) -> jedna koncova \0.
 * Bez vycpavky (bezny proces) 0 = neresit, cte se skutecny soubor. Limit:
 * proces, jehoz posledni argumenty jsou prazdne retezce, o ne prijde. */
static int pv_gen_cmdline_other(pv_w_t *w, const char *p, char *in) {
    long fd = shim_raw_syscall6(56, -100, (long)p, 0 | 02000000, 0, 0, 0);
    if (fd < 0) return 0;
    size_t have = 0;
    for (;;) {
        long r = shim_raw_syscall6(63, fd, (long)(in + have), (long)(PV_IN_CAP - have), 0, 0, 0);
        if (r <= 0) break;
        have += (size_t)r;
        if (have == PV_IN_CAP) { have = 0; break; }   /* moc velke: neresit */
    }
    shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
    if (have < 2 || in[have - 1] != 0 || in[have - 2] != 0) return 0;
    while (have > 1 && in[have - 2] == 0) have--;
    pv_putn(w, in, have);
    return 1;
}

static void pv_gen_cmdline(pv_w_t *w) {
    for (int i = 0; i < elf_init_argc && elf_init_argv && elf_init_argv[i]; i++) {
        pv_puts(w, elf_init_argv[i]);
        pv_putc(w, 0);
    }
}

/* Jeden radek maps (bez \n): "addr perms off dev inode   cesta". Cesta pod
 * $ROOTFS -> guest, mapovani loaderu -> guest exe. */
static void pv_maps_line(pv_w_t *w, char *line, size_t len, char *tmp) {
    size_t i = 0;
    for (int f = 0; f < 5 && i < len; f++) {          /* 5 poli + mezera */
        while (i < len && line[i] != ' ') i++;
        while (i < len && line[i] == ' ') i++;
    }
    if (i >= len || line[i] != '/') { pv_putn(w, line, len); return; }
    pv_putn(w, line, i);                              /* vc. zarovnani */
    char save = line[len];
    line[len] = 0;
    const char *path = line + i;
    size_t ll = g_shim_loader ? shim_strlen(g_shim_loader) : 0;
    if (ll && g_guest_exe_path[0] && shim_strncmp(path, g_shim_loader, ll) == 0 &&
        (path[ll] == 0 || path[ll] == ' ')) {
        pv_puts(w, pv_guestify(g_guest_exe_path, tmp, 4096, 1));
        pv_puts(w, path + ll);                        /* napr. " (deleted)" */
    } else {
        pv_puts(w, pv_guestify(path, tmp, 4096, 1));
    }
    line[len] = save;
}

static int pv_gen_maps(pv_w_t *w, char *in, char *tmp) {
    long fd = shim_raw_syscall6(56, -100, (long)"/proc/self/maps",
                                0 /* O_RDONLY */ | 02000000 /* O_CLOEXEC */, 0, 0, 0);
    if (fd < 0) return 0;
    size_t have = 0;
    for (;;) {
        long r = shim_raw_syscall6(63 /* read */, fd, (long)(in + have),
                                   (long)(PV_IN_CAP - have), 0, 0, 0);
        if (r < 0) { shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0); return 0; }
        have += (size_t)r;
        size_t s = 0;
        for (size_t k = 0; k < have; k++) {
            if (in[k] != '\n') continue;
            pv_maps_line(w, in + s, k - s, tmp);
            pv_putc(w, '\n');
            s = k + 1;
        }
        if (r == 0) {                                 /* EOF: zbytek bez \n */
            if (s < have) pv_maps_line(w, in + s, have - s, tmp);
            break;
        }
        if (s == 0 && have == PV_IN_CAP) {            /* radek > buffer: propust */
            pv_putn(w, in, have);
            have = 0;
            continue;
        }
        for (size_t k = s; k < have; k++) in[k - s] = in[k];
        have -= s;
    }
    shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
    return 1;
}

/* fstype/zdroj pro identity bind (dnesni exclude list) */
static const char *pv_fstype(const char *g) {
    if (shim_strcmp(g, "/proc") == 0) return "proc";
    if (shim_strcmp(g, "/sys") == 0) return "sysfs";
    if (shim_strcmp(g, "/dev") == 0 || shim_strcmp(g, "/dev/shm") == 0) return "tmpfs";
    return "none";
}

/* /proc/self/mounts (info=0) nebo mountinfo (info=1): koren + tabulka bindu */
static void pv_gen_mounts(pv_w_t *w, int info, char *tmp) {
    if (info) pv_puts(w, "1 0 0:1 / / rw,relatime - rootfs lxroot rw\n");
    else pv_puts(w, "lxroot / rootfs rw,relatime 0 0\n");
    if (!g_binds_ready || !g_tab) return;
    unsigned int cnt = __atomic_load_n(&g_tab->count, __ATOMIC_ACQUIRE);
    for (unsigned int i = 0; i < cnt && i < LX_BIND_MAX; i++) {
        lx_bind_t *b = &g_tab->e[i];
        if (!b->active) continue;
        int ident = bind_is_identity(b);
        const char *fst = ident ? pv_fstype(b->guest) : "none";
        const char *src = ident ? (shim_strcmp(fst, "none") ? fst : b->guest)
                                : pv_guestify(b->host, tmp, 4096, 0);
        /* /dev/shm je remap na $ROOTFS/dev/shm, ale pro guest je to tmpfs */
        if (!ident && shim_strcmp(b->guest, "/dev/shm") == 0) { fst = "tmpfs"; src = "tmpfs"; }
        const char *opt = b->ro ? "ro,relatime" : "rw,relatime";
        if (info) {
            pv_putu(w, i + 2); pv_puts(w, " 1 0:"); pv_putu(w, i + 2); pv_putc(w, ' ');
            pv_put_esc(w, ident ? "/" : src); pv_putc(w, ' ');
            pv_put_esc(w, b->guest); pv_putc(w, ' ');
            pv_puts(w, opt); pv_puts(w, " - "); pv_puts(w, fst); pv_putc(w, ' ');
            pv_put_esc(w, src); pv_putc(w, ' ');
            pv_puts(w, b->ro ? "ro" : "rw"); pv_putc(w, '\n');
        } else {
            pv_put_esc(w, src); pv_putc(w, ' ');
            pv_put_esc(w, b->guest); pv_putc(w, ' ');
            pv_puts(w, fst); pv_putc(w, ' ');
            pv_puts(w, opt); pv_puts(w, " 0 0\n");
        }
    }
}

/* memfd (fallback: soubor v $ROOTFS/tmp, hned unlink). Vraci fd nebo < 0. */
static long pv_memfd(int cloexec) {
    long fd = shim_raw_syscall6(279 /* memfd_create */, (long)"lxproc",
                                cloexec ? 1 /* MFD_CLOEXEC */ : 0, 0, 0, 0, 0);
    if (fd >= 0 || !g_shim_root || !g_shim_root[0]) return fd;
    static unsigned long ctr = 0;
    char p[1024];
    size_t rl = shim_strlen(g_shim_root);
    if (rl + 64 > sizeof p) return -1;
    shim_memcpy(p, g_shim_root, rl);
    shim_memcpy(p + rl, "/tmp/.lxproc.", 13);
    size_t k = rl + 13;
    k += (size_t)pv_utoa(p + k, (unsigned long)shim_raw_syscall6(178 /* gettid */, 0, 0, 0, 0, 0, 0));
    p[k++] = '.';
    pv_utoa(p + k, __atomic_fetch_add(&ctr, 1, __ATOMIC_RELAXED));
    fd = shim_raw_syscall6(56, -100, (long)p,
                           02 /* O_RDWR */ | 0100 /* O_CREAT */ | 0200 /* O_EXCL */ |
                           (cloexec ? 02000000 : 0), 0600, 0, 0);
    if (fd >= 0) shim_raw_syscall6(35 /* unlinkat */, -100, (long)p, 0, 0, 0, 0);
    return fd;
}

/* Hlavni vstup: 1 = obslouzeno (*fd_out = fd se syntetickym obsahem),
 * 0 = neresit (jit na skutecny soubor). Jen pro cteni. */
static int shim_proc_open(const char *p, int flags, long *fd_out) {
    if (!p || p[0] != '/' || g_cwd_keep_host) return 0;
    if ((flags & 03) != 0 || (flags & (0100 | 01000))) return 0;   /* O_ACCMODE, O_CREAT|O_TRUNC */
    int kind = pv_kind(p);
    if (kind == PV_NONE) return 0;
    if (kind == PV_CMDLINE && (!elf_init_argv || elf_init_argc <= 0)) return 0;
    long sz = PV_IN_CAP + PV_OUT_CAP + 4096;
    long m = shim_raw_syscall6(222 /* mmap */, 0, sz, 0x1 | 0x2, 0x20 | 0x02, -1, 0);
    if (m < 0 && m > -4096) return 0;
    char *in = (char *)m, *out = in + PV_IN_CAP, *tmp = out + PV_OUT_CAP;
    long fd = pv_memfd((flags & 02000000) != 0);
    int ok = 0;
    if (fd >= 0) {
        pv_w_t w = { fd, out, 0, 0 };
        if (kind == PV_CMDLINE) { pv_gen_cmdline(&w); ok = 1; }
        else if (kind == PV_CMDLINE_OTHER) ok = pv_gen_cmdline_other(&w, p, in);
        else if (kind == PV_MAPS) ok = pv_gen_maps(&w, in, tmp);
        else ok = (pv_gen_mounts(&w, kind == PV_MOUNTINFO, tmp), 1);
        pv_flush(&w);
        if (w.err) ok = 0;
        if (ok) shim_raw_syscall6(62 /* lseek */, fd, 0, 0 /* SEEK_SET */, 0, 0, 0);
        else shim_raw_syscall6(57 /* close */, fd, 0, 0, 0, 0, 0);
    }
    shim_raw_syscall6(215 /* munmap */, m, sz, 0, 0, 0, 0);
    if (!ok) return 0;
    *fd_out = fd;
    return 1;
}

/* readlink vysledek (b[0..r), bez NUL) pod $ROOTFS / remap bindem -> guest
 * cesta: /proc/self/fd/N, /proc/self/cwd, proot .l2s cile. Jako L1 l1_after.
 * Vraci novou delku. Bezi na normalnim zasobniku (jmenny hook). */
static ssize_t shim_readlink_guestify(char *b, size_t n, ssize_t r) {
    if (r <= 0 || b[0] != '/' || g_cwd_keep_host || (size_t)r >= 4096) return r;
    char h[4096], g[4096];
    shim_memcpy(h, b, (size_t)r);
    h[r] = 0;
    const char *s = pv_guestify(h, g, sizeof g, 1);
    size_t sl = shim_strlen(s);
    if (sl == (size_t)r && shim_strncmp(s, b, sl) == 0) return r;
    if (sl > n) sl = n;
    shim_memcpy(b, s, sl);
    return (ssize_t)sl;
}

/* Kernelova oblast argv (arg_start..arg_end, z ni cte /proc/<pid>/cmdline
 * i CIZI proces - ps, pgrep -f) obsahuje "elf_loader --argv0 X --ownall
 * /data/.../bin Y". Prepise ji na guest argv a ukazatele guestu presmeruje
 * do ni (setproctitle guestu se pak projevi jako v Linuxu). Zbytek oblasti
 * nuly - jadro je vrati taky (ps je ukaze jako mezery; limit). Kdyz se guest
 * argv nevejde, nic. Hranice [start, end) spocita main() HNED na zacatku
 * (guest argv je tataz pole jako argv z main(), dosazeni --argv0 by
 * souvislost rozbilo). Bezi pod BIONIC TP (z run_ownall pred elf_run). */
static void procvirt_rewrite_argarea(char *start, char *end, int gargc, char **gargv) {
    if (!start || !end || end <= start || gargc <= 0 || !gargv) return;
    size_t area = (size_t)(end - start), need = 0;
    for (int i = 0; i < gargc; i++) {
        if (!gargv[i]) return;
        need += strlen(gargv[i]) + 1;
    }
    if (elf_debug())
        fprintf(stderr, "[procvirt] argv oblast %zu B, guest %zu B (%d arg)\n", area, need, gargc);
    if (need > area) return;
    char *tmp = malloc(need);
    if (!tmp) return;
    size_t off = 0;
    for (int i = 0; i < gargc; i++) {
        size_t l = strlen(gargv[i]) + 1;
        memcpy(tmp + off, gargv[i], l);
        off += l;
    }
    memcpy(start, tmp, need);
    memset(start + need, 0, area - need);
    off = 0;
    for (int i = 0; i < gargc; i++) {
        gargv[i] = start + off;
        off += strlen(start + off) + 1;
    }
    free(tmp);
}

#endif /* ELF_LOADER_PROCVIRT_H */
