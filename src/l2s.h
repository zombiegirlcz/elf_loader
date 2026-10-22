/* src/l2s.h — prompt 08 (cast 2): link2symlink (ekvivalent proot -l).
 *
 * SELinux app domeny zakazuje hardlinky (AGENTS.md bod 8, "Hardlinky zakazuje
 * SELinux app domeny" — platformni limit). link()/linkat() tedy nelze delat
 * doslovne. Proot resi stejny problem pod -l: puvodni soubor presune do
 * skryteho uloziste a z OBOU jmen udela symlinky na nej; pocitadlo odkazu se
 * vede vedle (proot: PROOT_L2S_DIR). Tenhle header dela totez.
 *
 * Konvence (viz existujici komentare main.c/relsym.h/l1.h/procvirt.h, ktere
 * uz s ".l2s" cili pocitaly): skryty adresar "$ROOTFS/.l2s", cil symlinku je
 * HOSTOVE ABSOLUTNI cesta do nej (ne rootfs-relativni) — presne jako proot
 * .l2s cile, ktere uz loader umel cist (main.c shim_resolve_symlinks,
 * relsym.h, procvirt.h, l1.h). Pocitadlo odkazu je v "$ROOTFS/.l2s/<id>.nlink"
 * (textove desitkove cislo), jak zadava prompt 08 cast 2 a 08-fakeroot-l2s.md.
 *
 * POZNAMKA K PROOT KOMPATIBILITE (cestne priznani, nebyl cas verifikovat
 * binarne proti zdrojum proot bez pristupu k internetu v tomhle behu):
 * - presny nazev/format proot skryteho souboru (PROOT_L2S_DIR, pripadne
 *   soubor vedle originalu) NENI overen proti zdrojovym kodum proot (ENOENT
 *   na WebFetch/web nastroje v tomto behu). Prevzal jsem konvenci, kterou uz
 *   TENHLE projekt ocekaval (host-absolutni .l2s cile, viz komentare vyse) —
 *   to je jedina cast, kde je kompatibilita jista (loader uz umi CIST takove
 *   proot symlinky, protoze stejny tvar generujeme).
 * - Adresar: default "$ROOTFS/.l2s" (ne PROOT_L2S_DIR env, protoze ten by
 *   mohl ukazovat mimo rootfs a loader pracuje vyhradne pod ROOTFS). Pokud
 *   bude treba byt binarne kompatibilni s konkretni proot instalaci v appce,
 *   je potreba zjistit jeji skutecny PROOT_L2S_DIR (pripadne ho respektovat
 *   jako override — viz l2s_dir() nize, ktera cte LX_L2S_DIR env).
 * - id generujeme z (dev,ino) originalu v dekadickem tvaru "<dev>-<ino>",
 *   coz je proot-like (fakeroot/proot pouzivaji (dev,ino) jako klic), ale
 *   NENI to zaruceno byt bajtove tvar, ktery pouziva skutecny proot binary.
 *
 * Hot-path (l2s_link/l2s_unlink/l2s_fix_stat) bezi pod GUEST TP -> jen
 * shim_str-funkce a raw syscally (shim_raw_syscall6), zadne bionic
 * snprintf/getenv/velke buffery na stacku (AGENTS.md pasti 1 a 9).
 * l2s_init() bezi pri
 * startu pod bionickym TP (libc OK, vzor binds_init()).
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
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

/* raw syscall numbers (aarch64) pouzite v tomhle headeru */
#define L2S_SYS_UNLINKAT   35
#define L2S_SYS_SYMLINKAT  36
#define L2S_SYS_LINKAT     37
#define L2S_SYS_RENAMEAT2  276
#define L2S_SYS_READLINKAT 78
#define L2S_SYS_NEWFSTATAT 79
#define L2S_SYS_OPENAT     56
#define L2S_SYS_CLOSE      57
#define L2S_SYS_READ       63
#define L2S_SYS_WRITE      64
#define L2S_SYS_MKDIRAT    34
#define L2S_SYS_FTRUNCATE  46

static long shim_raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
static void shim_guest_errno_set(int v);
static size_t shim_strlen(const char *s);
static int shim_strncmp(const char *a, const char *b, size_t n);
static void shim_memcpy(void *d, const void *s, size_t n);

/* --- globalni stav, nastaveny jen v l2s_init() (bionicky TP) --- */
static int g_l2s_enabled = 0;           /* LX_L2S=1 */
static char g_l2s_dir[4096];            /* "$ROOTFS/.l2s", bez koncoveho '/' */
static int g_l2s_dir_len = 0;

/* Ceti env pri startu (bionicky TP, libc OK). Vzor binds_init(). */
static void l2s_init(void) {
    const char *v = getenv("LX_L2S");
    g_l2s_enabled = (v && v[0] == '1') ? 1 : 0;
    if (!g_l2s_enabled) return;
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

/* --- pomocne raw-syscall primitivy (guest TP safe) --- */

/* Manualni unsigned->decimal, bez snprintf. Pise do buf[0..], vraci delku
 * (bez NUL). buf musi mit >= 20 bajtu. */
static int l2s_u64_to_str(unsigned long v, char *buf) {
    char tmp[20]; int i = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0 && i < 20) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int len = i;
    for (int k = 0; k < len; k++) buf[k] = tmp[len - 1 - k];
    return len;
}
static int l2s_str_to_u64(const char *s, int len, unsigned long *out) {
    unsigned long v = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (unsigned long)(s[i] - '0');
    }
    *out = v;
    return 1;
}

/* id = "<dev>-<ino>" originalniho souboru (dev,ino jako klic, proot/fakeroot
 * styl). buf >= 64 bajtu. Vraci delku. */
static int l2s_make_id(unsigned long dev, unsigned long ino, char *buf, size_t bufsz) {
    (void)bufsz;
    int i = 0;
    i += l2s_u64_to_str(dev, buf + i);
    buf[i++] = '-';
    i += l2s_u64_to_str(ino, buf + i);
    buf[i] = 0;
    return i;
}

/* storage cesta "$g_l2s_dir/<id>" do out (>= 4096). */
static void l2s_storage_path(const char *id, int idlen, char *out) {
    shim_memcpy(out, g_l2s_dir, (size_t)g_l2s_dir_len);
    out[g_l2s_dir_len] = '/';
    shim_memcpy(out + g_l2s_dir_len + 1, id, (size_t)idlen);
    out[g_l2s_dir_len + 1 + idlen] = 0;
}
/* counter cesta "$g_l2s_dir/<id>.nlink" do out (>= 4096+6). */
static void l2s_counter_path(const char *id, int idlen, char *out) {
    l2s_storage_path(id, idlen, out);
    int l = g_l2s_dir_len + 1 + idlen;
    static const char suf[] = ".nlink";
    shim_memcpy(out + l, suf, sizeof(suf)); /* vc. NUL */
}

/* precte dekadicke cislo z maleho souboru (raw syscally). Vraci 1 pri
 * uspechu. Pouziva staticky buffer (slot) — bezpecne vuci AGENTS.md bod 9
 * (zadny buffer na STACKU zasobniku l1 stubu; tohle je ale volano jen z
 * jmennych hooku v main.c, ne z L1 stubu primo). */
static int l2s_counter_read(const char *path, unsigned long *out) {
    long fd = shim_raw_syscall6(L2S_SYS_OPENAT, AT_FDCWD, (long)(unsigned long)path, 0 /*O_RDONLY*/, 0, 0, 0);
    if (fd < 0) return 0;
    char buf[32];
    long n = shim_raw_syscall6(L2S_SYS_READ, fd, (long)(unsigned long)buf, sizeof(buf) - 1, 0, 0, 0);
    shim_raw_syscall6(L2S_SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    if (n <= 0) return 0;
    /* osekat pripadny trailing newline */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    return l2s_str_to_u64(buf, (int)n, out);
}
static int l2s_counter_write(const char *path, unsigned long v) {
    long fd = shim_raw_syscall6(L2S_SYS_OPENAT, AT_FDCWD, (long)(unsigned long)path,
                                 0101 /* O_WRONLY|O_CREAT (octal 0101 = 0x41) */, 0600, 0, 0);
    if (fd < 0) return 0;
    shim_raw_syscall6(L2S_SYS_FTRUNCATE, fd, 0, 0, 0, 0, 0);
    char buf[24];
    int len = l2s_u64_to_str(v, buf);
    long w = shim_raw_syscall6(L2S_SYS_WRITE, fd, (long)(unsigned long)buf, len, 0, 0, 0);
    shim_raw_syscall6(L2S_SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return w == len;
}

/* Je "path" (uz host-prelozena cesta pod ROOTFS) symlink smerujici do
 * g_l2s_dir? Pokud ano a target_out != NULL, zkopiruje cil (host-absolutni)
 * a vrati jeho delku; jinak 0. target_out musi mit >= 4096 bajtu. */
static int l2s_readlink_is_ours(const char *path, char *target_out) {
    if (!g_l2s_enabled || !g_l2s_dir_len) return 0;
    char tgt[4096];
    long n = shim_raw_syscall6(L2S_SYS_READLINKAT, AT_FDCWD, (long)(unsigned long)path,
                                (long)(unsigned long)tgt, sizeof(tgt) - 1, 0, 0);
    if (n <= 0) return 0;
    tgt[n] = 0;
    if (shim_strncmp(tgt, g_l2s_dir, (size_t)g_l2s_dir_len) != 0) return 0;
    if (tgt[g_l2s_dir_len] != '/') return 0;
    if (target_out) shim_memcpy(target_out, tgt, (size_t)n + 1);
    return (int)n;
}

/* --- hlavni operace, volane z main.c hooku --- */

/* link()/linkat() emulace. oldp/newp JSOU UZ host-prelozene cesty (po
 * shim_translate), presne jako vstup do g_orig_link. Vraci 0 pri uspechu
 * (volajici potom NEMA volat original — uz je hotovo), -1 + guest errno pri
 * chybe emulace samotne (volajici vrati -1 primo), nebo -2 kdyz se ma
 * pouzit puvodni cesta (l2s vypnuty / nepouzitelny — fallback na
 * g_orig_link, ktery stejne pod SELinuxem skonci EACCES — zachovava presne
 * puvodni chovani pri LX_L2S=0). */
static int l2s_link(const char *oldp, const char *newp) {
    if (!g_l2s_enabled) return -2;

    char existing_tgt[4096];
    int tl = l2s_readlink_is_ours(oldp, existing_tgt);
    if (tl > 0) {
        /* oldp uz je l2s jmeno -> novy symlink na stejny cil, nlink++ */
        long r = shim_raw_syscall6(L2S_SYS_SYMLINKAT, (long)(unsigned long)existing_tgt,
                                    AT_FDCWD, (long)(unsigned long)newp, 0, 0, 0);
        if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
        /* counter cesta = existing_tgt + ".nlink" */
        char cnt[4096 + 8];
        size_t el = shim_strlen(existing_tgt);
        shim_memcpy(cnt, existing_tgt, el);
        static const char suf[] = ".nlink";
        shim_memcpy(cnt + el, suf, sizeof(suf));
        unsigned long v = 1;
        l2s_counter_read(cnt, &v);
        l2s_counter_write(cnt, v + 1);
        return 0;
    }

    /* oldp je (predpokladem) obycejny soubor. Zjisti dev/ino. */
    struct stat st;
    long r = shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)oldp,
                                (long)(unsigned long)&st, AT_SYMLINK_NOFOLLOW, 0, 0);
    if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
    if ((st.st_mode & S_IFMT) != S_IFREG) return -2; /* jen regularni soubory */

    char id[64];
    int idlen = l2s_make_id((unsigned long)st.st_dev, (unsigned long)st.st_ino, id, sizeof id);
    char storage[4096];
    l2s_storage_path(id, idlen, storage);
    char counter[4096 + 8];
    l2s_counter_path(id, idlen, counter);

    /* presun originalu do uloziste (renameat2, bez prepsani) */
    r = shim_raw_syscall6(L2S_SYS_RENAMEAT2, AT_FDCWD, (long)(unsigned long)oldp,
                           AT_FDCWD, (long)(unsigned long)storage, RENAME_NOREPLACE, 0);
    if (r < 0) { shim_guest_errno_set((int)-r); return -1; }

    /* oldp -> symlink na storage (host-absolutni cil) */
    r = shim_raw_syscall6(L2S_SYS_SYMLINKAT, (long)(unsigned long)storage,
                           AT_FDCWD, (long)(unsigned long)oldp, 0, 0, 0);
    if (r < 0) {
        /* best-effort rollback: vrat original zpet */
        shim_raw_syscall6(L2S_SYS_RENAMEAT2, AT_FDCWD, (long)(unsigned long)storage,
                           AT_FDCWD, (long)(unsigned long)oldp, RENAME_NOREPLACE, 0);
        shim_guest_errno_set((int)-r); return -1;
    }
    /* newp -> symlink na storage */
    r = shim_raw_syscall6(L2S_SYS_SYMLINKAT, (long)(unsigned long)storage,
                           AT_FDCWD, (long)(unsigned long)newp, 0, 0, 0);
    if (r < 0) { shim_guest_errno_set((int)-r); return -1; }

    l2s_counter_write(counter, 2);
    return 0;
}

/* unlink()/unlinkat() hak: ma se zavolat PRED normalnim unlink() te same
 * cesty. Pokud "path" je l2s jmeno, snizi pocitadlo a pri 0 smaze i data +
 * counter soubor (samotne jmeno/symlink stejne pak smaze normalni unlink,
 * ktery nasleduje v puvodnim shim_unlink/shim_unlinkat — tenhle hak jen
 * udrzuje uloziste konzistentni). Nikdy nevraci chybu volajicimu — pri
 * jakekoli nejasnosti se jen nic neudela a normalni unlink probehne, jak
 * byl. */
static void l2s_unlink(const char *path) {
    if (!g_l2s_enabled) return;
    char tgt[4096];
    int tl = l2s_readlink_is_ours(path, tgt);
    if (tl <= 0) return;
    size_t el = (size_t)tl;
    char cnt[4096 + 8];
    shim_memcpy(cnt, tgt, el);
    static const char suf[] = ".nlink";
    shim_memcpy(cnt + el, suf, sizeof(suf));
    unsigned long v = 1;
    if (!l2s_counter_read(cnt, &v) || v == 0) return;
    if (v <= 1) {
        shim_raw_syscall6(L2S_SYS_UNLINKAT, AT_FDCWD, (long)(unsigned long)tgt, 0, 0, 0, 0);
        shim_raw_syscall6(L2S_SYS_UNLINKAT, AT_FDCWD, (long)(unsigned long)cnt, 0, 0, 0, 0);
    } else {
        l2s_counter_write(cnt, v - 1);
    }
}

/* stat/lstat/fstatat/statx oprava: "path" je host-prelozena cesta, "st" uz
 * obsahuje vysledek PUVODNIHO (originalniho) syscallu nad "path" s danymi
 * flags. follow = 1 znamena "puvodni volani symlink nasledovalo" (stat,
 * fstatat bez AT_SYMLINK_NOFOLLOW, statx bez stejneho flagu) — v tom pripade
 * uz kernel dosel na storage soubor sam (host-absolutni cil), takze
 * st_mode/st_size jsou spravne a staci jen prepsat st_nlink z citace.
 * follow = 0 (lstat, *at s AT_SYMLINK_NOFOLLOW) — "st" popisuje SYMLINK
 * samotny (S_IFLNK); l2s jmeno ale ma vypadat jako regularni soubor s danym
 * nlink, presne jako skutecny hardlink by vypadal — proto v tomhle pripade
 * musime stat zavolat znovu, tentokrat nad cilem, a to nahradit. */
static void l2s_fix_stat(const char *path, int follow, struct stat *st) {
    if (!g_l2s_enabled || !st) return;
    char tgt[4096];
    int tl = l2s_readlink_is_ours(path, tgt);
    if (tl <= 0) return;
    if (!follow) {
        struct stat real;
        long r = shim_raw_syscall6(L2S_SYS_NEWFSTATAT, AT_FDCWD, (long)(unsigned long)tgt,
                                    (long)(unsigned long)&real, 0, 0, 0);
        if (r < 0) return; /* nech puvodni (symlink) stat, lepsi nez nic */
        *st = real;
    }
    size_t el = (size_t)tl;
    char cnt[4096 + 8];
    shim_memcpy(cnt, tgt, el);
    static const char suf[] = ".nlink";
    shim_memcpy(cnt + el, suf, sizeof(suf));
    unsigned long v;
    if (l2s_counter_read(cnt, &v) && v > 0) st->st_nlink = (nlink_t)v;
}

#endif /* ELF_LOADER_L2S_H */
