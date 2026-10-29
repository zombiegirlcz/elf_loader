/* src/l1.h — L1 (prompt 04): preklad cest na HRANICI SYSCALLU.
 *
 * Dnesni vrstva hooku chyta podle JMENA funkce, takze se kazdy novy pripad
 * resi rucne (alias `eaccess`, BTI prolog, glibc-interni `bl`, `__mkdir`
 * v `mkdtemp`, `remove`, `glob`, `nftw`...) a kod, ktery `svc` vola sam
 * (staticke binarky, Go, Bun, JIT), nepokryje z principu. L1 patchuje
 * cestove `svc #0` v nactenych modulech na skok do per-site stubu, jehoz
 * telo (`l1_common` v entry.S) zavola odtud `l1_before` -> svc -> `l1_after`.
 *
 * Tenhle soubor se vklada PRIMO do main.c (pod shim_translate), protoze
 * pouziva jeho TLS-safe primitiva; `l1_before`/`l1_after` bezi pod GUEST TP,
 * takze nesmi volat bionic funkce zavisle na TLS (malloc, errno, snprintf).
 *
 * Zapina se `ELF_LOADER_L1=1` (dedi se do zanorenych exec), diagnostika
 * `ELF_LOADER_L1_TRACE=1`, statistika skenu `ELF_LOADER_L1_SCAN=1`.
 *
 * Vedome NEzahrnute syscally:
 *   - execve/execveat: exec neni "jen preklad cesty" - shim_execve resi
 *     own-load glibc binarek, raw exec bionickych, argv prestavbu. Patchnuti
 *     na urovni syscallu by glibc binarku spustilo RAW. Zustava L3 (jmenna
 *     vrstva), viz kapitola 2.3 studie.
 *   - mount: v app domene ENOSYS (seccomp), nema co prekladat.
 *   - bind/connect (AF_UNIX sun_path): vyzaduje kopii sockaddr, ne jen
 *     prepis ukazatele; jmenne shimy to uz resi. Dodelat az s L2.
 */
#ifndef ELF_LOADER_L1_H
#define ELF_LOADER_L1_H

/* ── tabulka cestovych syscallu (aarch64 asm-generic cisla) ──────────────
 * Vraci bitmasku: bit i = argument x_i je cesta, kterou je treba prelozit.
 * Je to zaroven jediny zdroj pravdy pro generator stubu (elf_loader.c
 * patchuje jen misto, jehoz nr tahle funkce zna) - staticke `mov x8,#nr`
 * tedy rozhoduje, ktera z 411 `svc` v libc se vubec diraji (merenim 48). */
unsigned l1_path_args(long nr) {
    switch (nr) {
    case 5: case 6: case 8: case 9:        /* [l]setxattr, [l]getxattr   */
    case 11: case 12: case 14: case 15:    /* [l]listxattr, [l]removexattr */
    case 43:                               /* statfs                     */
    case 45:                               /* truncate                   */
    case 49:                               /* chdir                      */
    case 51:                               /* chroot                     */
        return 1u << 0;
    case 27:                               /* inotify_add_watch          */
    case 33:                               /* mknodat                    */
    case 34:                               /* mkdirat                    */
    case 35:                               /* unlinkat                   */
    case 48:                               /* faccessat                  */
    case 53:                               /* fchmodat                   */
    case 54:                               /* fchownat                   */
    case 56:                               /* openat                     */
    case 78:                               /* readlinkat (+ fixup vysledku) */
    case 79:                               /* newfstatat                 */
    case 88:                               /* utimensat                  */
    case 291:                              /* statx                      */
    case 439:                              /* faccessat2                 */
        return 1u << 1;
    case 36:                               /* symlinkat: jen linkpath!   */
        return 1u << 2;                    /* x0 je CIL symlinku         */
    case 37:                               /* linkat                     */
    case 38:                               /* renameat                   */
    case 276:                              /* renameat2                  */
        return (1u << 1) | (1u << 3);
    case 17:                               /* getcwd: vstup zadny,       */
        return 0x80000000u;                /* jen fixup vysledku         */
    default:
        return 0;
    }
}

/* ── prekladove buffery ───────────────────────────────────────────────
 * Buffer NESMI byt na zasobniku: Go goroutines startuji s 8 KB a dve cesty
 * po PATH_MAX by ho snedly. Zadne TLS (bezime pod guest TP) a zadny malloc
 * (neni signal-safe a loaderuv malloc je bionicky).
 *
 * Slot se drzi jen po dobu jednoho stubu a jeho VLASTNIKEM je adresa ramce
 * (unikatni po dobu stubu, na kazdem vlakne i v kazde urovni zanoreni) -
 * ukazatel na slot si l1_before ulozi do frame[21] a l1_after ho uvolni.
 * Diky tomu nepotrebujeme `gettid` (extra syscall na KAZDY cestovy syscall
 * by L1 zdrazil dvakrat) ani urovne: signal guestu, ktery dorazi uprostred
 * stubu a udela dalsi cestovy syscall, si proste vezme jiny volny slot.
 *
 * Vlastnici jsou v KOMPAKTNIM poli na vlastni strance a hledani zacina
 * vzdy od nuly: jednovlaknovy proces tak pouziva jediny slot. Prvni verze
 * mela vlastnika uvnitr slotu a start odvozeny od adresy ramce - ruzna mista
 * volani pak sahala na ruzne sloty a RSS narostlo o ~320 KB misto ~90 KB. */
#define L1_SLOTS 256
#define L1_NPATH 2
#define L1_PATHMAX 4096
#define L1_OWNER_AREA 16384                /* >= stranka i pri 16K strankach */

typedef struct {
    char buf[L1_NPATH][L1_PATHMAX];
} l1_slot_t;

static volatile unsigned long *g_l1_owner = NULL;  /* [L1_SLOTS]; 0 = volny */
static l1_slot_t *g_l1_slots = NULL;               /* mmap NORESERVE, lazy */
static unsigned long g_l1_miss = 0;    /* kolikrat nebyl volny slot (diagnostika) */

/* Mapuje vlastniky + sloty. Vola se z main() pod bionickym TP, jednou. */
static int l1_slots_init(void) {
    if (g_l1_slots) return 1;
    size_t sz = L1_OWNER_AREA + sizeof(l1_slot_t) * L1_SLOTS;   /* ~2 MB virtualne */
    long r = shim_raw_syscall6(222 /* mmap */, 0, (long)sz,
                               0x1 | 0x2 /* RW */,
                               0x20 | 0x02 | 0x4000 /* ANON|PRIVATE|NORESERVE */,
                               -1, 0);
    if (r < 0 && r > -4096) return 0;
    g_l1_owner = (volatile unsigned long *)r;
    g_l1_slots = (l1_slot_t *)(r + L1_OWNER_AREA);
    return 1;
}

static l1_slot_t *l1_slot_acquire(unsigned long *frame) {
    if (!g_l1_slots) return NULL;
    unsigned long me = (unsigned long)frame;
    for (unsigned i = 0; i < L1_SLOTS; i++) {
        unsigned long zero = 0;
        if (g_l1_owner[i] == 0 &&
            __atomic_compare_exchange_n(&g_l1_owner[i], &zero, me, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return &g_l1_slots[i];
    }
    g_l1_miss++;
    return NULL;
}

static void l1_slot_release(l1_slot_t *s) {
    if (!s || !g_l1_slots) return;
    unsigned long i = (unsigned long)(s - g_l1_slots);
    if (i < L1_SLOTS) __atomic_store_n(&g_l1_owner[i], 0UL, __ATOMIC_RELEASE);
}

/* ── vlastni preklad ─────────────────────────────────────────────────────
 * Ramec (viz src/l1_common.S):
 *   frame[0..7]   PUVODNI x0..x7 (guest je po stubu dostane zpet)
 *   frame[8]      x8 = cislo syscallu
 *   frame[20]     vysledek syscallu (jen v l1_after)
 *   frame[21]     drzeny slot
 *   frame[22..29] argumenty, se kterymi jde syscall do jadra - jen tyhle
 *                 l1_before prepisuje
 * Idempotence resi shim_translate (cesta uz pod $ROOTFS se neprelozi), takze
 * L1 muze bezet SOUBEZNE s jmennymi hooky a dvojity prefix nevznikne. */
#define L1_ARG(f, i) ((f)[22 + (i)])

/* frame[21]: 0 = l1_after neni treba (l1_common ho vubec nezavola),
 * L1_NEED_AFTER = jen fixup vysledku bez slotu, jinak ukazatel na drzeny slot. */
#define L1_NEED_AFTER 1UL

/* Vraci 0 = provest syscall; jinak l1_common `svc` PRESKOCI a volajici dostane
 * frame[20] (shim_sys_intercept: odmitnuti ro/mountpoint bindem -EROFS/
 * -EBUSY, memfd pro /proc/self/cmdline...) - frame[21] pak zustava 0,
 * l1_after se nevola. */
long l1_before(unsigned long *frame, long nr) {
    unsigned mask = l1_path_args(nr);
    if (nr == 17 || nr == 78) frame[21] = L1_NEED_AFTER;   /* fixup vysledku */
    /* stat rodina pod fakerootem/l2s potrebuje opravu VYSLEDKU i tehdy, kdyz
     * se zadna cesta neprelozila (relativni, dirfd) — fallback "app uid -> fake
     * uid" zadnou cestu nepotrebuje. Bez tohohle davalo `L1=2 L2=1` st_uid
     * skutecneho app uid: L1 cestova `svc` prepise na stuby, takze L2 TRAP
     * (a s nim `go_fakeroot_post`) uz nenastane (namereno 2026-10-29). */
    if ((nr == 79 || nr == 291) && (g_fakeroot_on || g_l2s_enabled))
        frame[21] = L1_NEED_AFTER;
    if (!(mask & 0xffu)) return 0;                 /* nic na vstupu */
    /* Rychla cesta: zadny ABSOLUTNI cestovy argument (find/fts jedou pres
     * dirfd + jmeno) -> nic k prekladu, zadny slot, zadny CAS a l1_after se
     * nevola. Na `find /usr` je takovych volani ~300 tisic. */
    int any = 0;
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1u << i))) continue;
        const char *p = (const char *)L1_ARG(frame, i);
        if (p && p[0] == '/') { any = 1; break; }
    }
    if (!any) return 0;
    /* ro bind / mountpoint / virtualni /proc: z PUVODNICH (guest) cest */
    long res;
    if (shim_sys_intercept(nr, &L1_ARG(frame, 0), &res)) {
        frame[20] = (unsigned long)res;
        frame[21] = 0;
        return 1;
    }
    l1_slot_t *s = l1_slot_acquire(frame);
    if (!s) return 0;                              /* jde neprelozene dal */
    frame[21] = (unsigned long)s;                  /* l1_after ho uvolni */
    int used = 0;
    for (int i = 0; i < 8 && used < L1_NPATH; i++) {
        if (!(mask & (1u << i))) continue;
        const char *p = (const char *)L1_ARG(frame, i);
        if (!p || p[0] != '/') continue;           /* relativni -> dirfd resi jadro */
        char *out = s->buf[used];
        if (shim_translate(p, out, L1_PATHMAX)) {
            L1_ARG(frame, i) = (unsigned long)out;
            used++;
        }
    }
    /* symlinkat: absolutni guest cil zapis relativne (invariant rootfs,
     * src/relsym.h) - jako jmenny shim_symlinkat, aby L1 bez jmennych hooku
     * nedelal nic jineho. Hloubku linku umime jen u AT_FDCWD. */
    if (nr == 36 && used < L1_NPATH && (long)L1_ARG(frame, 1) == -100 &&
        L1_ARG(frame, 0) && L1_ARG(frame, 2) &&
        ((const char *)L1_ARG(frame, 2))[0] == '/') {  /* jen absolutni: bez bufferu na zasobniku */
        char *out = s->buf[used];
        if (shim_abs_target_to_rel((const char *)L1_ARG(frame, 0),
                                   (const char *)L1_ARG(frame, 2), out, L1_PATHMAX))
            L1_ARG(frame, 0) = (unsigned long)out;
    }
    return 0;
}

/* Odrizne prefix $ROOTFS z vysledku, ktery jadro naplnilo hostovou cestou.
 * `len` = delka (getcwd vcetne NUL, readlinkat bez NUL), vraci novou. */
static long l1_strip_root_buf(char *buf, long len, int with_nul) {
    size_t rl = (g_shim_root && g_shim_root[0]) ? shim_strlen(g_shim_root) : 0;
    if (!rl || !buf || len <= 0 || (size_t)len < rl) return len;
    if (shim_strncmp(buf, g_shim_root, rl) != 0) return len;
    long body = len - (with_nul ? 1 : 0);          /* znaky bez NUL */
    if ((size_t)body > rl && buf[rl] != '/') return len;   /* .../parrotX */
    long nb = body - (long)rl;                      /* zbytek za prefixem */
    if (nb <= 0) {                                  /* presne koren */
        buf[0] = '/';
        if (with_nul) { buf[1] = 0; return 2; }
        return 1;
    }
    for (long i = 0; i < nb; i++) buf[i] = buf[rl + i];
    if (with_nul) { buf[nb] = 0; return nb + 1; }
    return nb;
}

/* Opravy VYSLEDKU: guest nesmi nikdy uvidet hostovou cestu. Prave tohle
 * dela "stripovani $R" u glob/nftw/getcwd zbytecnym - kdyz se preklada az
 * na urovni syscallu, program cely cas pracuje s guest cestami.
 *   - getcwd(17): jadro naplnilo x0-buffer, vysledek = delka VCETNE NUL.
 *   - readlinkat(78): cil muze byt hostove absolutni (proot .l2s) nebo
 *     kanonicky relativni (relsym) -> oboje ohlasime jako guest-absolutni.
 * Vyjimku pro Bun drzi stejny priznak jako jmenny hook (`g_cwd_keep_host`:
 * Bun dela stat/open raw syscally a guest cesta pro nej neexistuje). */
void l1_after(unsigned long *frame, long nr) {
    long ret = (long)frame[20];

    if (ret > 0 && !g_cwd_keep_host) {
        if (nr == 17) {
            /* remap bind (mount --bind) -> guest prefix, jinak strip $ROOTFS */
            size_t rv = bind_reverse_inplace((char *)frame[0], (size_t)frame[1]);
            frame[20] = rv ? (unsigned long)(rv + 1)
                           : (unsigned long)l1_strip_root_buf((char *)frame[0], ret, 1);
        } else if (nr == 78) {
            char *b = (char *)frame[2];
            size_t cap = (size_t)frame[3];
            /* /proc/self/exe ukazuje na LOADER - stejna vyjimka jako jmenny
             * shim_readlinkat (cmake a dalsi self-introspekce) */
            if ((long)frame[0] == -100 && g_guest_exe_path[0] &&
                shim_is_proc_self_exe((const char *)frame[1])) {
                size_t len = shim_strlen(g_guest_exe_path);
                if (len > cap) len = cap;
                shim_memcpy(b, g_guest_exe_path, len);
                frame[20] = (unsigned long)len;
                goto release;
            }
            ret = l1_strip_root_buf(b, ret, 0);
            /* kanonicky relativni cil -> absolutni (jen AT_FDCWD + absolutni
             * prelozena cesta linku, jinak hloubku nezname). Cil se kvuli NUL
             * kopiruje do buf[1] DRZENEHO slotu - jmenny shim_readlink_fixup
             * by si vzal 8 KB zasobniku. */
            l1_slot_t *s = frame[21] > L1_NEED_AFTER ? (l1_slot_t *)frame[21] : NULL;
            const char *lp = (const char *)L1_ARG(frame, 1);
            if (s && ret > 0 && (size_t)ret < L1_PATHMAX && b[0] != '/' &&
                (long)frame[0] == -100 && lp && lp[0] == '/' && !g_no_relsym) {
                char *tgt = s->buf[1];
                shim_memcpy(tgt, b, (size_t)ret);
                tgt[ret] = 0;
                if (shim_rel_target_to_abs(tgt, lp, b, cap))
                    ret = (long)shim_strlen(b);
            }
            frame[20] = (unsigned long)ret;
        }
    }

    /* fakeroot / l2s oprava vysledku stat rodiny. Cestu pouzijeme jen kdyz je
     * AT_FDCWD + absolutni (tedy uz prelozena); jinak NULL — `fakeroot_fix_*`
     * pak udela jen fallback na fake uid a `l2s_fix_*` se vynecha. Zadny
     * buffer na zasobniku: l1_after bezi na cizim 8 KB stacku (past c. 10). */
    if (ret == 0 && (nr == 79 || nr == 291) && (g_fakeroot_on || g_l2s_enabled)) {
        long dfd = (long)L1_ARG(frame, 0);
        const char *tp = (const char *)L1_ARG(frame, 1);
        int usable = (dfd == -100 && tp && tp[0] == '/');
        if (nr == 79) {
            int follow = !((int)L1_ARG(frame, 3) & 0x100 /* AT_SYMLINK_NOFOLLOW */);
            struct stat *st = (struct stat *)L1_ARG(frame, 2);
            fakeroot_fix_stat(usable ? tp : NULL, follow, st);
            if (usable) l2s_fix_stat(tp, follow, st);
        } else {
            int follow = !((int)L1_ARG(frame, 2) & 0x100);
            void *stx = (void *)L1_ARG(frame, 4);
            fakeroot_fix_statx(usable ? tp : NULL, follow, stx);
            if (usable)
                l2s_fix_statx(tp, follow, (int)dfd, (unsigned int)L1_ARG(frame, 3), stx);
        }
    }

release:
    if (frame[21] > L1_NEED_AFTER)                 /* slot drzi jen l1_before */
        l1_slot_release((l1_slot_t *)frame[21]);
    frame[21] = 0;
}

#endif /* ELF_LOADER_L1_H */
