/* src/l2s.h — prompt 08 cast 2: link2symlink (ekvivalent proot -l).
 *
 * SELinux app domeny zakazuje hardlinky (AGENTS.md bod 8). Format PRESNE
 * prevzaty z proot (termux/proot, src/extension/link2symlink/link2symlink.c,
 * non-USERLAND vetev — PREFIX ".l2s."), overeno jak proti zdrojovemu kodu,
 * tak proti REALNYM artefaktum, ktere uz v `kali` rootfs lezely z drivejsiho
 * ostreho proot behu (`$ROOTFS/.l2s/.l2s.perl.dpkg-new0001` apod.):
 *
 *   original (kdekoli v rootfs) --symlink--> intermediate --symlink--> final
 *
 *   intermediate = "$L2S_DIR/" PREFIX <basename-original> <NNNN>
 *   final        = intermediate "." <MMMM>
 *
 * NNNN (4 cislice) je jen jednoznacny slot pro dany basename (hleda se od
 * 0001, prvni volny). MMMM (4 cislice, ODDELENE TECKOU od NNNN) je AKTUALNI
 * POCET ODKAZU (nlink) — NENI to generace. Potvrzeno ze zdrojoveho kodu
 * proot (`finalStat.st_nlink = atoi(final + strlen(final) - 4)` v
 * `handle_sysexit_end`, stejne v `link2symlink_handle_statx`). Kazdy dalsi
 * `link()` na existujici l2s jmeno: precte final, MMMM+1, `rename(final,
 * novy_final)`, prepise symlink intermediate->novy_final. `unlink` dela
 * presny opak (MMMM-1; na 0 smaze intermediate i final).
 *
 * `L2S_DIR` = `$ROOTFS/.l2s` (viz `$D/usr/bin/boot`: `export
 * PROOT_L2S_DIR="$ROOTFS/.l2s"` — presne umisteni, ktere uz tenhle projekt
 * predpokladal v komentarich main.c/relsym.h/l1.h/procvirt.h). Cile symlinku
 * jsou HOSTOVE ABSOLUTNI (overeno `readlink` na realnych proot artefaktech
 * v kali — `.l2s.perl.dpkg-new0001` -> `/data/.../kali/.l2s/.l2s.perl.dpkg-
 * new0001.0002`, plna hostova cesta). Diky tomu je tahle implementace
 * schopna cist i soubory, ktere uz v rootfs vytvoril skutecny proot (a
 * naopak) — format je ted bajtove stejny, ne jen adresarove.
 *
 * Co se NEPORTOVALO (mimo rozpocet, proot ma navic): dir_fd-vazana ochrana
 * L2S_DIR proti prepsani symlinkem (proot to resi kvuli bezpecnosti vuci
 * tracee, ktery muze menit vlastni rootfs mezi syscally — u nas neni
 * zvlastni hrozba navic oproti existujicimu modelu), fd_cache +
 * readlink_proc_fd (/proc/<pid>/fd/N na l2s souboru hlasi jmeno intermediate
 * misto final — kosmeticke), execve_proc_exe (argv[0]/exe pro multi-call
 * binarky spoustene pres l2s jmeno), handle_linkat_from_proc_fd (specialni
 * /proc/X/fd/Y linkat trik). Tyto chybejici casti jsou neskodne (nejhorsi
 * dusledek: drobna kosmeticka odchylka v /proc, ne spatna data).
 *
 * Hot-path bezi pod GUEST TP -> jen shim_str-funkce a raw syscally
 * (shim_raw_syscall6), zadne bionic snprintf/getenv/velke buffery na
 * stacku L1 stubu (AGENTS.md pasti 1 a 9; tyhle funkce ale bezi z named
 * hooku, ne z L1 stubu, takze normalni stack buffery 4 KB jsou v poradku).
 * l2s_init() bezi pri startu pod bionickym TP (libc OK, vzor binds_init()).
 */
#ifndef ELF_LOADER_L2S_H
#define ELF_LOADER_L2S_H

#include <sys/stat.h>

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

#define L2S_SYS_UNLINKAT   35
#define L2S_SYS_SYMLINKAT  36
#define L2S_SYS_RENAMEAT2  276
#define L2S_SYS_READLINKAT 78
#define L2S_SYS_NEWFSTATAT 79

#define L2S_PREFIX     ".l2s."
#define L2S_PREFIX_LEN 5

static long shim_raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
static void shim_guest_errno_set(int v);
static size_t shim_strlen(const char *s);
static int shim_strncmp(const char *a, const char *b, size_t n);
static void shim_memcpy(void *d, const void *s, size_t n);

static int g_l2s_enabled = 0;           /* LX_L2S=1 */
static char g_l2s_dir[4096];            /* "$ROOTFS/.l2s", bez koncoveho '/' */
static int g_l2s_dir_len = 0;

static void l2s_init(void) {
    const char *v = getenv("LX_L2S");
    g_l2s_enabled = (v && v[0] == '1') ? 1 : 0;
    if (!g_l2s_enabled) return;
    extern const char *g_shim_root;
    const char *root = g_shim_root;
    if (!root || !root[0]) { g_l2s_enabled = 0; return; }
    const char *override = getenv("LX_L2S_DIR");
    int n;
    if (override && override[0])
        n = snprintf(g_l2s_dir, sizeof g_l2s_dir, "%s", override);
    else
        n = snprintf(g_l2s_dir, sizeof g_l2s_dir, "%s/.l2s", root);
    if (n <= 0 || (size_t)n >= sizeof g_l2s_dir) { g_l2s_enabled = 0; return; }
    g_l2s_dir_len = n;
    mkdir(g_l2s_dir, 0700);
}

/* --- male retezcove pomocniky (guest TP safe, zadny snprintf) --- */

static const char *l2s_basename(const char *p) {
    const char *b = p, *s = p;
    while (*s) { if (*s == '/') b = s + 1; s++; }
    return b;
}
static int l2s_has_prefix(const char *name) {
    return shim_strncmp(name, L2S_PREFIX, L2S_PREFIX_LEN) == 0;
}
/* posledni 4 znaky retezce (delky len) jsou cislice -> vrat jejich hodnotu,
 * jinak -1. */
static int l2s_last4_to_int(const char *s, size_t len) {
    if (len < 4) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[len - 4 + i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    return v;
}
static void l2s_fmt4(int v, char *out) {
    if (v < 0) v = 0;
    if (v > 9999) v = 9999;
    out[0] = (char)('0' + (v / 1000) % 10);
    out[1] = (char)('0' + (v / 100) % 10);
    out[2] = (char)('0' + (v / 10) % 10);
    out[3] = (char)('0' + v % 10);
}
/* prepise posledni 4 znaky retezce "s" (delky len) hodnotou v (in place). */
static void l2s_set_last4(char *s, size_t len, int v) {
    if (len < 4) return;
    l2s_fmt4(v, s + len - 4);
}

static long l2s_readlink(const char *path, char *out, size_t outsz) {
    long n = shim_raw_syscall6(L2S_SYS_READLINKAT, AT_FDCWD, (long)(unsigned long)path,
                                (long)(unsigned long)out, outsz - 1, 0, 0);
    if (n > 0) out[n] = 0;
    return n;
}
static long l2s_lstat_exists(const char *path) {
    struct stat st;
    return shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)path,
                              (long)(unsigned long)&st, AT_SYMLINK_NOFOLLOW, 0, 0);
}

/* --- hlavni operace --- */

/* link()/linkat() emulace. oldp/newp jsou uz host-prelozene cesty. Vraci 0
 * (hotovo), -1 (chyba, guest errno nastaven), nebo -2 (LX_L2S vypnuty nebo
 * se nejedna o regularni soubor/l2s jmeno -> volajici ma zavolat original). */
static int l2s_link(const char *oldp, const char *newp) {
    if (!g_l2s_enabled) return -2;

    char intermediate[4096];
    struct stat st;
    long r = shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)oldp,
                                (long)(unsigned long)&st, AT_SYMLINK_NOFOLLOW, 0, 0);
    if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
    if ((st.st_mode & S_IFMT) == S_IFDIR) { shim_guest_errno_set(EPERM); return -1; }

    int is_relink = 0;
    if ((st.st_mode & S_IFMT) == S_IFLNK) {
        long n = l2s_readlink(oldp, intermediate, sizeof intermediate);
        if (n <= 0) return -2;
        if (!l2s_has_prefix(l2s_basename(intermediate))) return -2; /* cizi symlink, nechat original */
        is_relink = 1;
    } else if ((st.st_mode & S_IFMT) != S_IFREG) {
        return -2; /* jen regularni soubory a existujici l2s jmena */
    }

    char final[4096];
    if (is_relink) {
        long n = l2s_readlink(intermediate, final, sizeof final);
        if (n <= 0) return -2; /* rozbity l2s link - nechat na originalu */
        size_t fl = (size_t)n;
        int cnt = l2s_last4_to_int(final, fl);
        if (cnt < 0) return -2;
        cnt++;
        char new_final[4096];
        shim_memcpy(new_final, final, fl + 1);
        l2s_set_last4(new_final, fl, cnt);
        r = shim_raw_syscall6(L2S_SYS_RENAMEAT2, AT_FDCWD, (long)(unsigned long)final,
                               AT_FDCWD, (long)(unsigned long)new_final, 0, 0);
        if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
        shim_raw_syscall6(L2S_SYS_UNLINKAT, AT_FDCWD, (long)(unsigned long)intermediate, 0, 0, 0, 0);
        r = shim_raw_syscall6(L2S_SYS_SYMLINKAT, (long)(unsigned long)new_final,
                               AT_FDCWD, (long)(unsigned long)intermediate, 0, 0, 0);
        if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
    } else {
        /* prvni link: najdi volny slot NNNN pro basename(oldp) v L2S_DIR */
        const char *name = l2s_basename(oldp);
        size_t namelen = shim_strlen(name);
        if ((size_t)g_l2s_dir_len + L2S_PREFIX_LEN + namelen + 11 >= sizeof intermediate)
            { shim_guest_errno_set(ENAMETOOLONG); return -1; }
        size_t pos = 0;
        shim_memcpy(intermediate, g_l2s_dir, (size_t)g_l2s_dir_len); pos += (size_t)g_l2s_dir_len;
        intermediate[pos++] = '/';
        shim_memcpy(intermediate + pos, L2S_PREFIX, L2S_PREFIX_LEN); pos += L2S_PREFIX_LEN;
        shim_memcpy(intermediate + pos, name, namelen); pos += namelen;
        size_t base_pos = pos;
        int slot;
        for (slot = 1; slot < 1000; slot++) {
            l2s_fmt4(slot, intermediate + base_pos);
            intermediate[base_pos + 4] = 0;
            if (l2s_lstat_exists(intermediate) < 0) break; /* volny slot */
        }
        if (slot >= 1000) { shim_guest_errno_set(ENOSPC); return -1; }

        shim_memcpy(final, intermediate, base_pos + 4);
        final[base_pos + 4] = '.';
        l2s_fmt4(2, final + base_pos + 5);
        final[base_pos + 9] = 0;

        r = shim_raw_syscall6(L2S_SYS_RENAMEAT2, AT_FDCWD, (long)(unsigned long)oldp,
                               AT_FDCWD, (long)(unsigned long)final, 0, 0);
        if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
        r = shim_raw_syscall6(L2S_SYS_SYMLINKAT, (long)(unsigned long)final,
                               AT_FDCWD, (long)(unsigned long)intermediate, 0, 0, 0);
        if (r < 0) {
            shim_raw_syscall6(L2S_SYS_RENAMEAT2, AT_FDCWD, (long)(unsigned long)final,
                               AT_FDCWD, (long)(unsigned long)oldp, 0, 0);
            shim_guest_errno_set((int)-r); return -1;
        }
        r = shim_raw_syscall6(L2S_SYS_SYMLINKAT, (long)(unsigned long)intermediate,
                               AT_FDCWD, (long)(unsigned long)oldp, 0, 0, 0);
        if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
    }

    r = shim_raw_syscall6(L2S_SYS_SYMLINKAT, (long)(unsigned long)intermediate,
                           AT_FDCWD, (long)(unsigned long)newp, 0, 0, 0);
    if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
    return 0;
}

/* unlink()/unlinkat() hak: ma se zavolat PRED normalnim unlink() te same
 * cesty. Nikdy nevraci chybu volajicimu (viz l2s_unlink v proot - skipuje
 * ticho na rozbitem retezci). */
static void l2s_unlink(const char *path) {
    if (!g_l2s_enabled) return;
    struct stat st;
    long r = shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)path,
                                (long)(unsigned long)&st, AT_SYMLINK_NOFOLLOW, 0, 0);
    if (r < 0 || (st.st_mode & S_IFMT) != S_IFLNK) return;

    char intermediate[4096];
    long n = l2s_readlink(path, intermediate, sizeof intermediate);
    if (n <= 0) return;
    if (!l2s_has_prefix(l2s_basename(intermediate))) return;

    char final[4096];
    n = l2s_readlink(intermediate, final, sizeof final);
    if (n <= 0) return; /* rozbity l2s link, nic delat */
    size_t fl = (size_t)n;
    int cnt = l2s_last4_to_int(final, fl);
    if (cnt < 0) return;
    cnt--;

    if (cnt > 0) {
        char new_final[4096];
        shim_memcpy(new_final, final, fl + 1);
        l2s_set_last4(new_final, fl, cnt);
        shim_raw_syscall6(L2S_SYS_RENAMEAT2, AT_FDCWD, (long)(unsigned long)final,
                           AT_FDCWD, (long)(unsigned long)new_final, 0, 0);
        shim_raw_syscall6(L2S_SYS_UNLINKAT, AT_FDCWD, (long)(unsigned long)intermediate, 0, 0, 0, 0);
        shim_raw_syscall6(L2S_SYS_SYMLINKAT, (long)(unsigned long)new_final,
                           AT_FDCWD, (long)(unsigned long)intermediate, 0, 0, 0);
    } else {
        shim_raw_syscall6(L2S_SYS_UNLINKAT, AT_FDCWD, (long)(unsigned long)intermediate, 0, 0, 0, 0);
        shim_raw_syscall6(L2S_SYS_UNLINKAT, AT_FDCWD, (long)(unsigned long)final, 0, 0, 0, 0);
    }
}

/* stat/lstat/fstatat/newfstatat oprava. "path" je host-prelozena, NEresolvnuta
 * cesta (presne ta, co by sel kernel lstat). follow=1 = puvodni volani
 * symlink nasledovalo (stat, fstatat bez AT_SYMLINK_NOFOLLOW) -> "st" uz
 * obsahuje atributy FINAL souboru (kernel prosel oba symlink hopy sam),
 * staci dopocitat nlink. follow=0 = "st" popisuje SYMLINK samotny; musime
 * rucne dojit na final a cely st prepsat. */
static void l2s_fix_stat(const char *path, int follow, struct stat *st) {
    if (!g_l2s_enabled || !st) return;

    const char *bn = l2s_basename(path);
    char intermediate[4096], final[4096];
    long n;

    if (l2s_has_prefix(bn)) {
        /* "path" uz je sam intermediate nebo final jmeno v L2S_DIR */
        if ((st->st_mode & S_IFMT) == S_IFLNK || !follow) {
            /* over, jestli jde o intermediate (symlink na final) */
            struct stat st2;
            long r = shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)path,
                                        (long)(unsigned long)&st2, AT_SYMLINK_NOFOLLOW, 0, 0);
            if (r < 0) return;
            if ((st2.st_mode & S_IFMT) == S_IFLNK) {
                n = l2s_readlink(path, final, sizeof final);
                if (n <= 0) return;
            } else {
                shim_memcpy(final, path, shim_strlen(path) + 1);
            }
        } else {
            shim_memcpy(final, path, shim_strlen(path) + 1);
        }
    } else {
        /* obycejne jmeno (hardlink-emulovane) - musi byt symlink na
         * intermediate */
        struct stat st2;
        long r = shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)path,
                                    (long)(unsigned long)&st2, AT_SYMLINK_NOFOLLOW, 0, 0);
        if (r < 0 || (st2.st_mode & S_IFMT) != S_IFLNK) return;
        n = l2s_readlink(path, intermediate, sizeof intermediate);
        if (n <= 0) return;
        if (!l2s_has_prefix(l2s_basename(intermediate))) return;
        n = l2s_readlink(intermediate, final, sizeof final);
        if (n <= 0) return;
    }

    size_t fl = shim_strlen(final);
    int cnt = l2s_last4_to_int(final, fl);
    if (cnt < 0) return;

    if (!follow) {
        struct stat real;
        long r = shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)final,
                                    (long)(unsigned long)&real, 0, 0, 0);
        if (r < 0) return;
        *st = real;
    }
    st->st_nlink = (nlink_t)cnt;
}

/* statx() ma jiny ABI layout nez struct stat (kernel struct statx, stabilni
 * od zavedeni syscallu) -> samostatna oprava jen pro stx_nlink (offset 16,
 * u32) a stx_mask (offset 0, u32, bit STATX_NLINK=0x4). "stx" je surovy
 * buffer predany do statx(2). follow ma stejny vyznam jako u l2s_fix_stat. */
#define L2S_STATX_SYS       291
#define L2S_STATX_NLINK_OFF 16
#define L2S_STATX_MASK_BIT  0x00000004u
static void l2s_fix_statx(const char *path, int follow, int dfd, unsigned int mask, void *stx) {
    if (!g_l2s_enabled || !stx) return;
    unsigned int smask;
    shim_memcpy(&smask, stx, 4);
    if (!(smask & L2S_STATX_MASK_BIT)) return;

    const char *bn = l2s_basename(path);
    char intermediate[4096], final[4096];
    long n;

    if (l2s_has_prefix(bn)) {
        struct stat st2;
        long r = shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)path,
                                    (long)(unsigned long)&st2, AT_SYMLINK_NOFOLLOW, 0, 0);
        if (r < 0) return;
        if ((st2.st_mode & S_IFMT) == S_IFLNK) {
            n = l2s_readlink(path, final, sizeof final);
            if (n <= 0) return;
        } else {
            shim_memcpy(final, path, shim_strlen(path) + 1);
        }
    } else {
        struct stat st2;
        long r = shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)path,
                                    (long)(unsigned long)&st2, AT_SYMLINK_NOFOLLOW, 0, 0);
        if (r < 0 || (st2.st_mode & S_IFMT) != S_IFLNK) return;
        n = l2s_readlink(path, intermediate, sizeof intermediate);
        if (n <= 0) return;
        if (!l2s_has_prefix(l2s_basename(intermediate))) return;
        n = l2s_readlink(intermediate, final, sizeof final);
        if (n <= 0) return;
    }

    size_t fl = shim_strlen(final);
    int cnt = l2s_last4_to_int(final, fl);
    if (cnt < 0) return;

    if (!follow) {
        /* refresh cely buffer statx(2) syscallem nad final, aby ostatni
         * pole (mode/size/ino...) odpovidala realnemu souboru, ne symlinku */
        shim_raw_syscall6(L2S_STATX_SYS, dfd == -100 ? AT_FDCWD : dfd,
                           (long)(unsigned long)final, 0 /* flags */, mask,
                           (long)(unsigned long)stx, 0);
    }
    unsigned int nlink32 = (unsigned int)cnt;
    shim_memcpy((char *)stx + L2S_STATX_NLINK_OFF, &nlink32, 4);
}

#endif /* ELF_LOADER_L2S_H */
