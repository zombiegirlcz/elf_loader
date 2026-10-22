/* src/fakeroot.h — prompt 08 cast 1: fake root (ekvivalent `proot -0`).
 *
 * Za LX_FAKEROOT=1:
 *   - getuid/geteuid/getgid/getegid/getresuid/getresgid/getgroups -> 0
 *   - stat/lstat/fstat/fstatat/newfstatat/statx: vlastnik app uid/gid se
 *     hlasi jako 0:0; pokud je na inode ulozen xattr override (chown na jiny
 *     uid/gid, setuid/setgid bit, nebo mknod zarizeni), prepise se st_uid/
 *     st_gid/st_mode/st_rdev z nej.
 *   - chown/lchown/fchown/fchownat: vzdy uspech, pozadovane uid/gid se zapise
 *     do xattr (realny chown se NEVOLA - app proces nemuze chownovat na cizi
 *     uid, a neni to potreba, staci falsifikovat stat).
 *   - chmod/fchmod/fchmodat se setuid/setgid bity: realny chmod se zavola BEZ
 *     techto bitu (SELinux/app domena by 04xxx/02xxx mohla odmitnout nebo je
 *     tise zahodit), plny pozadovany mode (vc. bitu) se zapise do xattr.
 *   - mknod/mknodat zarizeni (S_IFCHR/S_IFBLK): vytvori se obycejny prazdny
 *     soubor a do xattr se zapise typ + rdev; stat pak hlasi spravny typ.
 *
 * DB rozhodnuti (viz postup.md 2026-10-22 (5)): f2fs na /data POVOLUJE
 * uzivatelske xattry (user.*) pro app soubory i pod app UID (overeno
 * python os.setxattr/os.getxattr na kali). Pouzivame proto primo xattr
 * "user.lxfakeroot" na kazdem souboru/adresari - zadny sidecar soubor,
 * zadna DB, zadny (dev,ino) klic. Vyhoda oproti sidecaru: xattr je soucast
 * inode, takze rename/unlink/link ji automaticky "presunou"/zahodi spravne
 * (zadna rucni udrzba konzistence, zadne ino-reuse problemy).
 *
 * Format xattr hodnoty (binarni, fixed-size, host byte order - jen aarch64):
 *   u32 magic ("LXFR"), u32 uid, u32 gid, u32 mode_extra (jen SUID/SGID/STICKY
 *   bity + pro mknod S_IFCHR/S_IFBLK), u32 rdev_major, u32 rdev_minor.
 *
 * KRITICKE: tento kod bezi pod GUEST (glibc) TP -> jen raw syscally
 * (shim_raw_syscall6) a shim_str.../shim_mem... helpery z main.c. Zadne bionic
 * snprintf/getenv/getpid/malloc. Env (LX_FAKEROOT, app uid) se cte jednou
 * pri startu pod bionickym TP (fakeroot_init, vzor binds_init).
 */
#ifndef ELF_LOADER_FAKEROOT_H
#define ELF_LOADER_FAKEROOT_H

#include <sys/stat.h>
#include <sys/xattr.h>

/* forward declarace z main.c (definovana dale v souboru) - potrebujeme
 * nastavit guest errno po neuspesnem raw syscallu, jinak volajici uvidi
 * nahodnou starou hodnotu errno (dpkg "Illegal seek" misto skutecne
 * priciny). */
static void shim_guest_errno_set(int v);

#define LXFR_XATTR_NAME "user.lxfakeroot"
#define LXFR_MAGIC 0x4c584652u /* "LXFR" */

typedef struct {
    unsigned int magic;
    unsigned int uid;
    unsigned int gid;
    unsigned int mode_extra; /* S_ISUID|S_ISGID|S_ISVTX, + S_IFCHR/S_IFBLK pro mknod */
    unsigned int rdev_major;
    unsigned int rdev_minor;
} lxfr_rec_t;

static int g_fakeroot_on = 0;
static unsigned int g_fakeroot_app_uid = 0xFFFFFFFFu;
static unsigned int g_fakeroot_app_gid = 0xFFFFFFFFu;

/* fakeroot_init bezi PRED prvnim execem guest kodu, pod bionickym TP
 * (stejne misto jako binds_init v main.c) - normalni getenv/getuid OK. */
static void fakeroot_init(void) {
    const char *v = getenv("LX_FAKEROOT");
    g_fakeroot_on = (v && v[0] && v[0] != '0') ? 1 : 0;
    if (g_fakeroot_on) {
        g_fakeroot_app_uid = (unsigned int)getuid();
        g_fakeroot_app_gid = (unsigned int)getgid();
    }
}

/* --- raw syscall wrappery pro guest TP (fsetxattr=7, fgetxattr=10 na arm64;
 * ale my pracujeme s CESTAMI, ne fd, takze pouzivame setxattr/getxattr
 * primo pres bionic libc call - tyto funkce NEJSOU TLS-zavisle (nepouzivaji
 * errno pres __thread na nejake nekompatibilni misto), ale pro jistotu a
 * konzistenci s projektovym pravidlem "jen raw syscall pod guest TP"
 * pouzijeme raw syscall cislo primo. aarch64: setxattr=5, getxattr=8,
 * lsetxattr=6, lgetxattr=9. */
#ifndef __NR_setxattr
#define __NR_setxattr  5
#define __NR_getxattr  8
#define __NR_lsetxattr 6
#define __NR_lgetxattr 9
#define __NR_fsetxattr 7
#define __NR_fgetxattr 10
#endif

static int fakeroot_xattr_set(const char *path, int follow, const lxfr_rec_t *rec) {
    long nr = follow ? __NR_setxattr : __NR_lsetxattr;
    long r = shim_raw_syscall6(nr, (long)path, (long)LXFR_XATTR_NAME,
                                (long)rec, (long)sizeof(*rec), 0, 0);
    if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
    return 0;
}

static int fakeroot_xattr_get(const char *path, int follow, lxfr_rec_t *rec) {
    long nr = follow ? __NR_getxattr : __NR_lgetxattr;
    long r = shim_raw_syscall6(nr, (long)path, (long)LXFR_XATTR_NAME,
                                (long)rec, (long)sizeof(*rec), 0, 0);
    if (r != (long)sizeof(*rec) || rec->magic != LXFR_MAGIC) return -1;
    return 0;
}

/* Precte existujici zaznam (nebo nulovy), zmeni pozadovana pole a zapise. */
static int fakeroot_xattr_merge(const char *path, int follow,
                                 const unsigned int *uid, const unsigned int *gid,
                                 const unsigned int *mode_extra,
                                 const unsigned int *rmaj, const unsigned int *rmin) {
    lxfr_rec_t rec;
    if (fakeroot_xattr_get(path, follow, &rec) != 0) {
        rec.magic = LXFR_MAGIC; rec.uid = 0; rec.gid = 0;
        rec.mode_extra = 0; rec.rdev_major = 0; rec.rdev_minor = 0;
    }
    if (uid) rec.uid = *uid;
    if (gid) rec.gid = *gid;
    if (mode_extra) rec.mode_extra = *mode_extra;
    if (rmaj) rec.rdev_major = *rmaj;
    if (rmin) rec.rdev_minor = *rmin;
    return fakeroot_xattr_set(path, follow, &rec);
}

/* fakeroot_fix_stat — JEDNA funkce ktera upravuje struct stat podle fake-root
 * pravidel. Volat po uspesnem realnem stat/lstat/fstat/fstatat/statx (na
 * PRELOZENE/resolvnute ceste - path muze byt NULL pro fstat-only pripady,
 * kde xattr nelze precist cestou; v tom pripade se jen uid/gid 0 falsifikuje
 * bez device/setuid rozsireni). follow = 1 pro stat/fstatat bez
 * AT_SYMLINK_NOFOLLOW (xattr na cili symlinku), 0 pro lstat.
 *
 * POZNAMKA pro link2symlink mergy: tato funkce je jediny bod, ktery sahá do
 * struct stat polí st_uid/st_gid/st_mode/st_rdev - link2symlink agent, ktery
 * meni st_nlink, by sem nemel zasahovat (jiny field). */
static void fakeroot_fix_stat(const char *path, int follow, struct stat *st) {
    if (!g_fakeroot_on || !st) return;
    lxfr_rec_t rec; int have_rec = path ? (fakeroot_xattr_get(path, follow, &rec) == 0) : 0;

    if (have_rec) {
        st->st_uid = rec.uid;
        st->st_gid = rec.gid;
        if (rec.mode_extra & (S_IFCHR | S_IFBLK)) {
            /* mknod zarizeni: byl vytvoren jako obycejny soubor, hlasime
             * spravny typ + rdev */
            mode_t perm = st->st_mode & 07777;
            st->st_mode = (rec.mode_extra & (S_IFCHR | S_IFBLK)) | perm |
                          (rec.mode_extra & (S_ISUID | S_ISGID | S_ISVTX));
#if defined(__GLIBC__) || 1
            st->st_rdev = (dev_t)((((unsigned long)rec.rdev_major) << 8) |
                                    ((unsigned long)rec.rdev_minor & 0xffu) |
                                    ((((unsigned long)rec.rdev_minor) & ~0xffu) << 12));
#endif
        } else {
            st->st_mode = (st->st_mode & ~(mode_t)(S_ISUID | S_ISGID | S_ISVTX)) |
                          (rec.mode_extra & (S_ISUID | S_ISGID | S_ISVTX));
        }
    } else if (st->st_uid == g_fakeroot_app_uid) {
        /* zadny xattr override - vlastnik je app uid -> hlasime root:root
         * (vychozi proot -0 chovani pro soubory, ktere jsme sami vytvorili) */
        st->st_uid = 0;
        if (st->st_gid == g_fakeroot_app_gid) st->st_gid = 0;
    }
}

static void fakeroot_fix_stat_fd(int fd, struct stat *st) {
    if (!g_fakeroot_on || !st || fd < 0) return;
    lxfr_rec_t rec;
    long r = shim_raw_syscall6(__NR_fgetxattr, fd, (long)LXFR_XATTR_NAME,
                                (long)&rec, (long)sizeof(rec), 0, 0);
    int have_rec = (r == (long)sizeof(rec) && rec.magic == LXFR_MAGIC);
    if (have_rec) {
        st->st_uid = rec.uid; st->st_gid = rec.gid;
        if (rec.mode_extra & (S_IFCHR | S_IFBLK)) {
            mode_t perm = st->st_mode & 07777;
            st->st_mode = (rec.mode_extra & (S_IFCHR | S_IFBLK)) | perm |
                          (rec.mode_extra & (S_ISUID | S_ISGID | S_ISVTX));
            st->st_rdev = (dev_t)((((unsigned long)rec.rdev_major) << 8) |
                                    ((unsigned long)rec.rdev_minor & 0xffu) |
                                    ((((unsigned long)rec.rdev_minor) & ~0xffu) << 12));
        } else {
            st->st_mode = (st->st_mode & ~(mode_t)(S_ISUID | S_ISGID | S_ISVTX)) |
                          (rec.mode_extra & (S_ISUID | S_ISGID | S_ISVTX));
        }
    } else if (st->st_uid == g_fakeroot_app_uid) {
        st->st_uid = 0;
        if (st->st_gid == g_fakeroot_app_gid) st->st_gid = 0;
    }
}

static int fakeroot_xattr_fmerge(int fd, const unsigned int *uid, const unsigned int *gid,
                                  const unsigned int *mode_extra,
                                  const unsigned int *rmaj, const unsigned int *rmin) {
    lxfr_rec_t rec;
    long r = shim_raw_syscall6(__NR_fgetxattr, fd, (long)LXFR_XATTR_NAME,
                                (long)&rec, (long)sizeof(rec), 0, 0);
    if (r != (long)sizeof(rec) || rec.magic != LXFR_MAGIC) {
        rec.magic = LXFR_MAGIC; rec.uid = 0; rec.gid = 0;
        rec.mode_extra = 0; rec.rdev_major = 0; rec.rdev_minor = 0;
    }
    if (uid) rec.uid = *uid;
    if (gid) rec.gid = *gid;
    if (mode_extra) rec.mode_extra = *mode_extra;
    if (rmaj) rec.rdev_major = *rmaj;
    if (rmin) rec.rdev_minor = *rmin;
    r = shim_raw_syscall6(__NR_fsetxattr, fd, (long)LXFR_XATTR_NAME,
                           (long)&rec, (long)sizeof(rec), 0, 0);
    if (r < 0) { shim_guest_errno_set((int)-r); return -1; }
    return 0;
}

/* --- identita procesu: getuid/geteuid/... -> 0 za LX_FAKEROOT ---------- */
static uid_t fakeroot_getuid(void)  { return g_fakeroot_on ? 0 : getuid(); }
static uid_t fakeroot_geteuid(void) { return g_fakeroot_on ? 0 : geteuid(); }
static gid_t fakeroot_getgid(void)  { return g_fakeroot_on ? 0 : getgid(); }
static gid_t fakeroot_getegid(void) { return g_fakeroot_on ? 0 : getegid(); }

#endif /* ELF_LOADER_FAKEROOT_H */
