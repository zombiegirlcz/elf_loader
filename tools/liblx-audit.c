/* liblx-audit.so - preklad cest v NATIVNIM glibc procesu (prompt 12, PoC).
 *
 * Mechanismus
 * -----------
 * V nativnim modu neinstalujeme hooky sami - udela to ld.so za nas. Pres
 * rtld-audit rozhrani vratime z `la_symbind64` adresu vlastniho wrapperu,
 * takze kazda PLT vazba programu na cestovou funkci libc skonci u nas.
 * Je to tentyz ucinek jako L3 jmenne hooky loaderu, ale bez GOT override,
 * bez `hook_install` a bez BTI problemu (past c. 2) - vazbu prepisuje linker.
 *
 * Druhy podstatny rozdil proti own-loadu: v procesu je JEDNA libc, takze
 * tady NEPLATI past c. 1 (guest TP vs bionicky TLS). Wrappery smi volat
 * cokoli z libc vcetne `snprintf` - zadne `dl_enter_host`, zadne raw syscally.
 *
 * Co to NEpokryva (a proto je to PoC, ne mod)
 * ------------------------------------------
 *   - libc-interni volani (`bl` uvnitr libc) PLT neprochazi - stejna mez
 *     jako u jmennych hooku, resi se az L1 prepisem `svc` v textu;
 *   - raw syscally (Go, Bun, staticke binarky) - na ty je L2;
 *   - cesty v jinych rodinach funkci, nez je tabulka nize.
 * Smysl PoC je ukazat, ze hakovaci bod v nativnim modu existuje a za kolik.
 *
 * Build (rootfs gcc, NE bionic):
 *     gcc -O2 -fPIC -shared -o liblx-audit.so tools/liblx-audit.c
 *
 * Pouziti:
 *     lxnative $R/lib/.../ld-linux-aarch64.so.1 --library-path … \
 *              --audit /cesta/liblx-audit.so <prog> [args…]
 *     LX_ROOTFS=$R     koren, ktery se lepi pred absolutni cesty
 *     LX_AUDIT_TRACE=1 vypisovat prelozene cesty na stderr
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *g_root;
static size_t g_rootlen;
static int g_trace;

/* Cesty, ktere do rootfs NEpatri - musi zustat hostove. Je to tentyz seznam,
 * jako ma loader v `shim_excluded` / tabulce bindu (prompt 06). */
static const char *const g_host_prefix[] = {
    "/proc", "/sys", "/dev", "/data", "/system", "/apex", "/vendor",
    "/odm", "/product", "/acct", "/storage", "/mnt", "/debug_ramdisk",
};

static int is_host_path(const char *p)
{
    for (unsigned i = 0; i < sizeof g_host_prefix / sizeof g_host_prefix[0]; i++) {
        size_t n = strlen(g_host_prefix[i]);
        if (strncmp(p, g_host_prefix[i], n) == 0 && (p[n] == 0 || p[n] == '/'))
            return 1;
    }
    return 0;
}

/* Vrati bud `p` (nic neprekladame), nebo `buf` s nalepenym rootfs. */
static const char *xlat(const char *p, char *buf, size_t bufsz)
{
    if (!p || p[0] != '/' || !g_root)
        return p;
    if (strncmp(p, g_root, g_rootlen) == 0 && (p[g_rootlen] == 0 || p[g_rootlen] == '/'))
        return p;                               /* uz je prelozena */
    if (is_host_path(p))
        return p;
    if (g_rootlen + strlen(p) + 1 > bufsz)
        return p;
    memcpy(buf, g_root, g_rootlen);
    strcpy(buf + g_rootlen, p);
    if (g_trace)
        fprintf(stderr, "[liblx] %s -> %s\n", p, buf);
    return buf;
}

#define XL(p) char _b[PATH_MAX]; const char *_p = xlat((p), _b, sizeof _b)

/* --- skutecne funkce libc, adresu dostaneme z la_symbind64 --- */
static int   (*r_open)(const char *, int, ...);
static int   (*r_openat)(int, const char *, int, ...);
static int   (*r_stat)(const char *, struct stat *);
static int   (*r_lstat)(const char *, struct stat *);
static int   (*r_statx)(int, const char *, int, unsigned int, void *);
static DIR  *(*r_opendir)(const char *);
static int   (*r_access)(const char *, int);
static int   (*r_chdir)(const char *);
static char *(*r_getcwd)(char *, size_t);
static int   (*r_unlink)(const char *);
static int   (*r_readlink_)(const char *, char *, size_t);
static int   (*r_readlinkat)(int, const char *, char *, size_t);

static int w_open(const char *p, int fl, mode_t m)   { XL(p); return r_open(_p, fl, m); }
static int w_openat(int fd, const char *p, int fl, mode_t m)
                                                     { XL(p); return r_openat(fd, _p, fl, m); }
static int w_stat(const char *p, struct stat *st)    { XL(p); return r_stat(_p, st); }
static int w_lstat(const char *p, struct stat *st)   { XL(p); return r_lstat(_p, st); }
static int w_statx(int fd, const char *p, int fl, unsigned int m, void *b)
                                                     { XL(p); return r_statx(fd, _p, fl, m, b); }
static DIR *w_opendir(const char *p)                 { XL(p); return r_opendir(_p); }
static int w_access(const char *p, int m)            { XL(p); return r_access(_p, m); }
static int w_chdir(const char *p)                    { XL(p); return r_chdir(_p); }
static int w_unlink(const char *p)                   { XL(p); return r_unlink(_p); }

/* `/proc/self/exe` v nativnim modu ukazuje na ld.so, ne na program - ld.so je
 * totiz skutecne to, co jadro spustilo. Opravit to musime, jinak kazdy, kdo
 * z `/proc/self/exe` odvozuje svuj prefix, sahne mimo (naopak python tim
 * nahodou najde stdlib, protoze ld.so lezi v rootfs - viz postup.md). */
static const char *g_prog;

static int is_self_exe(const char *p)
{
    if (strcmp(p, "/proc/self/exe") == 0)
        return 1;
    if (strncmp(p, "/proc/", 6) != 0)
        return 0;
    char mine[32];
    int n = snprintf(mine, sizeof mine, "/proc/%ld/exe", (long)getpid());
    return n > 0 && n < (int)sizeof mine && strcmp(p, mine) == 0;
}

static int self_exe_into(char *b, size_t n)
{
    size_t l = strlen(g_prog);
    if (l > n)
        l = n;
    memcpy(b, g_prog, l);
    return (int)l;                      /* readlink NEterminuje nulou */
}

static int w_readlink(const char *p, char *b, size_t n)
{
    if (g_prog && p && is_self_exe(p))
        return self_exe_into(b, n);
    XL(p);
    return r_readlink_(_p, b, n);
}

static int w_readlinkat(int fd, const char *p, char *b, size_t n)
{
    if (g_prog && p && is_self_exe(p))
        return self_exe_into(b, n);
    XL(p);
    return r_readlinkat(fd, _p, b, n);
}

/* --- exec shim -----------------------------------------------------------
 * Bez nej se nativni mod chova nebezpecne: guest `sh -c "/bin/ls"` nespadne,
 * ale TISE spusti hostovy `/system/bin/ls` (Android ma `/bin` -> `/system/bin`),
 * takze `ls --version` hlasi "toybox 0.8.6-android". Zmereno - proto je tohle
 * povinna cast, ne vylepseni.
 *
 * Dite zustava pod ptrace tracerem (`lxnative`) i po execve, takze mu SIGSYS
 * absorber zustava. Staci tedy prepsat prikazovou radku na spusteni ld.so.  */
extern char **environ;

static int (*r_execve)(const char *, char *const[], char *const[]);
static int (*r_execv)(const char *, char *const[]);
static int (*r_execvp)(const char *, char *const[]);

/* Ma soubor PT_INTERP? Takovou binarku musi spustit ld.so, protoze jadro by
 * jeji interpret (`/lib/ld-linux-aarch64.so.1`) hledalo v HOSTOVEM korenu. */
/* Cte se zamerne pres `syscall()`, ne pres `open`/`read`: `open` je v nasi
 * tabulce, takze volani z teto knihovny by se vratilo do `w_open` a pripadne
 * do jeste nenavazaneho `r_open` (NULL). `syscall` v tabulce neni. */
static int needs_ldso(const char *path)
{
    long fd = syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    Elf64_Ehdr eh;
    int ok = 0;
    if (syscall(SYS_read, fd, &eh, sizeof eh) == (long)sizeof eh &&
        memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
        eh.e_ident[EI_CLASS] == ELFCLASS64 && eh.e_phnum > 0 &&
        eh.e_phnum < 128 &&
        syscall(SYS_lseek, fd, (long)eh.e_phoff, SEEK_SET) >= 0) {
        Elf64_Phdr ph;
        for (int i = 0; i < eh.e_phnum; i++) {
            if (syscall(SYS_read, fd, &ph, sizeof ph) != (long)sizeof ph)
                break;
            if (ph.p_type != PT_INTERP || ph.p_filesz == 0 || ph.p_filesz > 256)
                continue;
            /* Nestaci, ze PT_INTERP existuje - hostove bionic binarky ho maji
             * taky (`/system/bin/linker64`) a pres glibc ld.so se spustit
             * nedaji. Zmereno: bez teto kontroly skonci `ls` z PATH na
             * "libcrypto.so: cannot open shared object file". */
            char interp[257];
            if (syscall(SYS_lseek, fd, (long)ph.p_offset, SEEK_SET) < 0)
                break;
            long rd = syscall(SYS_read, fd, interp, ph.p_filesz);
            if (rd <= 0)
                break;
            interp[rd < 256 ? rd : 256] = 0;
            ok = strstr(interp, "ld-linux") != NULL;
            break;
        }
    }
    syscall(SYS_close, fd);
    return ok;
}

static int raw_execve(const char *p, char *const a[], char *const e[])
{
    if (r_execve)
        return r_execve(p, a, e);
    return (int)syscall(SYS_execve, p, a, e);
}

#define MAX_ARGV 512

static int exec_shim(const char *path, char *const argv[], char *const envp[])
{
    char b[PATH_MAX];
    const char *real = xlat(path, b, sizeof b);
    const char *ldso = getenv("LX_LDSO");
    const char *lpath = getenv("LX_LIBPATH");
    const char *audit = getenv("LX_AUDIT");

    if (!ldso || !lpath || !needs_ldso(real))
        return raw_execve(real, argv, envp);     /* bionic / staticka / skript */

    int n = 0;
    while (argv[n] && n < MAX_ARGV - 8)
        n++;
    if (argv[n])
        return raw_execve(real, argv, envp);     /* prilis dlouhe argv */

    const char *na[MAX_ARGV];
    int k = 0;
    na[k++] = ldso;
    na[k++] = "--argv0";
    na[k++] = argv[0] ? argv[0] : path;
    na[k++] = "--library-path";
    na[k++] = lpath;
    if (audit && *audit) {
        na[k++] = "--audit";
        na[k++] = audit;
    }
    na[k++] = real;
    for (int i = 1; i < n; i++)
        na[k++] = argv[i];
    na[k] = NULL;
    if (g_trace)
        fprintf(stderr, "[liblx] exec %s pres ld.so\n", real);
    return raw_execve(ldso, (char *const *)na, envp);
}

static int w_execve(const char *p, char *const a[], char *const e[])
{
    return exec_shim(p, a, e);
}

static int w_execv(const char *p, char *const a[])
{
    return exec_shim(p, a, environ);
}

/* execvp s holym jmenem by vyzadovalo prohledani PATH v guestu - to PoC
 * nedela a necha to na libc (ta pak najde hostovou binarku). Dokumentovana
 * mez, ne tichy uspech: s lomitkem se chova spravne. */
static int w_execvp(const char *p, char *const a[])
{
    if (strchr(p, '/'))
        return exec_shim(p, a, environ);
    return r_execvp(p, a);
}

/* Opacny smer: hostovou cestu pod rootfs prevest na guestovou. Bez toho
 * `pwd`/`realpath` vraci absolutni host cestu. Pracuje na miste - guestova
 * podoba je vzdy kratsi, takze se do bufferu vejde. */
static char *unxlat(char *s)
{
    if (!s || !g_root)
        return s;
    if (strncmp(s, g_root, g_rootlen) != 0)
        return s;
    const char *rest = s + g_rootlen;
    if (*rest == 0)
        strcpy(s, "/");
    else
        memmove(s, rest, strlen(rest) + 1);
    return s;
}

static char *w_getcwd(char *buf, size_t n)
{
    return unxlat(r_getcwd(buf, n));
}

/* --- dalsi cestove rodiny ------------------------------------------------- */
static int   (*r_fstatat)(int, const char *, struct stat *, int);
static int   (*r_faccessat)(int, const char *, int, int);
static int   (*r_unlinkat)(int, const char *, int);
static int   (*r_rename)(const char *, const char *);
static int   (*r_mkdir)(const char *, mode_t);
static int   (*r_rmdir)(const char *);
static int   (*r_chmod)(const char *, mode_t);
static int   (*r_symlink)(const char *, const char *);
static int   (*r_utimensat)(int, const char *, const void *, int);
static char *(*r_realpath)(const char *, char *);
static char *(*r_mkdtemp)(char *);
static int   (*r_mkstemp)(char *);
static FILE *(*r_fopen)(const char *, const char *);
static void *(*r_dlopen)(const char *, int);

static int w_fstatat(int fd, const char *p, struct stat *st, int fl)
                                                     { XL(p); return r_fstatat(fd, _p, st, fl); }
static int w_faccessat(int fd, const char *p, int m, int fl)
                                                     { XL(p); return r_faccessat(fd, _p, m, fl); }
static int w_unlinkat(int fd, const char *p, int fl) { XL(p); return r_unlinkat(fd, _p, fl); }
static int w_mkdir(const char *p, mode_t m)          { XL(p); return r_mkdir(_p, m); }
static int w_rmdir(const char *p)                    { XL(p); return r_rmdir(_p); }
static int w_chmod(const char *p, mode_t m)          { XL(p); return r_chmod(_p, m); }
static int w_utimensat(int fd, const char *p, const void *t, int fl)
                                                     { XL(p); return r_utimensat(fd, _p, t, fl); }
static FILE *w_fopen(const char *p, const char *m)   { XL(p); return r_fopen(_p, m); }

static int w_rename(const char *a, const char *b)
{
    char ba[PATH_MAX], bb[PATH_MAX];
    return r_rename(xlat(a, ba, sizeof ba), xlat(b, bb, sizeof bb));
}

/* Cil symlinku se NEPREKLADA - zustava guestovy, aby rootfs zustal prenosny
 * a aby fungoval i pod prootem (past c. 7, src/relsym.h). */
static int w_symlink(const char *target, const char *link)
{
    char b[PATH_MAX];
    return r_symlink(target, xlat(link, b, sizeof b));
}

static char *w_realpath(const char *p, char *out)
{
    char b[PATH_MAX];
    char *r = r_realpath(xlat(p, b, sizeof b), out);
    return unxlat(r);
}

/* mkdtemp/mkstemp menia sablonu na miste a vraci ji volajicimu. Prelozenou
 * cestu proto po volani zase zkratime zpet na guestovou podobu - ta je vzdy
 * kratsi, takze se do puvodniho bufferu vejde. Bez toho skonci `/tmp/x.NNN`
 * v HOSTOVEM /tmp, ktere na Androidu neexistuje. */
static char *w_mkdtemp(char *tmpl)
{
    char b[PATH_MAX];
    const char *t = xlat(tmpl, b, sizeof b);
    if (t == tmpl)
        return r_mkdtemp(tmpl);
    char *r = r_mkdtemp(b);
    if (!r)
        return NULL;
    strcpy(tmpl, b + g_rootlen);
    return tmpl;
}

static int w_mkstemp(char *tmpl)
{
    char b[PATH_MAX];
    const char *t = xlat(tmpl, b, sizeof b);
    if (t == tmpl)
        return r_mkstemp(tmpl);
    int fd = r_mkstemp(b);
    if (fd >= 0)
        strcpy(tmpl, b + g_rootlen);
    return fd;
}

static void *w_dlopen(const char *p, int fl)
{
    if (!p)
        return r_dlopen(p, fl);
    XL(p);
    return r_dlopen(_p, fl);
}

struct entry { const char *name; void *wrap; void **real; };

static const struct entry g_tab[] = {
    { "open",     w_open,     (void **)&r_open     },
    { "open64",   w_open,     (void **)&r_open     },
    { "openat",   w_openat,   (void **)&r_openat   },
    { "openat64", w_openat,   (void **)&r_openat   },
    { "stat",     w_stat,     (void **)&r_stat     },
    { "stat64",   w_stat,     (void **)&r_stat     },
    { "lstat",    w_lstat,    (void **)&r_lstat    },
    { "lstat64",  w_lstat,    (void **)&r_lstat    },
    { "statx",    w_statx,    (void **)&r_statx    },
    { "opendir",  w_opendir,  (void **)&r_opendir  },
    { "access",   w_access,   (void **)&r_access   },
    { "chdir",    w_chdir,    (void **)&r_chdir    },
    { "getcwd",   w_getcwd,   (void **)&r_getcwd   },
    { "unlink",   w_unlink,   (void **)&r_unlink   },
    { "readlink", w_readlink, (void **)&r_readlink_ },
    { "execve",   w_execve,   (void **)&r_execve   },
    { "execv",    w_execv,    (void **)&r_execv    },
    { "execvp",   w_execvp,   (void **)&r_execvp   },
    { "readlinkat", w_readlinkat, (void **)&r_readlinkat },
    { "fstatat",  w_fstatat,  (void **)&r_fstatat  },
    { "fstatat64", w_fstatat, (void **)&r_fstatat  },
    { "faccessat", w_faccessat, (void **)&r_faccessat },
    { "unlinkat", w_unlinkat, (void **)&r_unlinkat },
    { "rename",   w_rename,   (void **)&r_rename   },
    { "mkdir",    w_mkdir,    (void **)&r_mkdir    },
    { "rmdir",    w_rmdir,    (void **)&r_rmdir    },
    { "chmod",    w_chmod,    (void **)&r_chmod    },
    { "symlink",  w_symlink,  (void **)&r_symlink  },
    { "utimensat", w_utimensat, (void **)&r_utimensat },
    { "realpath", w_realpath, (void **)&r_realpath },
    { "mkdtemp",  w_mkdtemp,  (void **)&r_mkdtemp  },
    { "mkstemp",  w_mkstemp,  (void **)&r_mkstemp  },
    { "mkstemp64", w_mkstemp, (void **)&r_mkstemp  },
    { "fopen",    w_fopen,    (void **)&r_fopen    },
    { "fopen64",  w_fopen,    (void **)&r_fopen    },
    { "dlopen",   w_dlopen,   (void **)&r_dlopen   },
};

unsigned int la_version(unsigned int v)
{
    g_root = getenv("LX_ROOTFS");
    if (g_root && !*g_root)
        g_root = NULL;
    g_rootlen = g_root ? strlen(g_root) : 0;
    g_trace = getenv("LX_AUDIT_TRACE") != NULL;
    g_prog = getenv("LX_PROG");
    if (g_prog && !*g_prog)
        g_prog = NULL;
    return v;
}

unsigned int la_objopen(struct link_map *map, Lmid_t lmid, uintptr_t *cookie)
{
    (void)lmid;
    *cookie = (uintptr_t)map;           /* la_symbind64 z nej spocita adresu */
    return LA_FLG_BINDFROM | LA_FLG_BINDTO;
}

uintptr_t la_symbind64(Elf64_Sym *sym, unsigned int ndx, uintptr_t *refcook,
                       uintptr_t *defcook, unsigned int *flags, const char *symname)
{
    (void)ndx; (void)refcook; (void)flags;
    if (!g_root || !symname)
        return sym->st_value;
    for (unsigned i = 0; i < sizeof g_tab / sizeof g_tab[0]; i++) {
        if (strcmp(symname, g_tab[i].name) != 0)
            continue;
        /* POZOR: glibc do `sym` predava KOPII, ve ktere uz `st_value` je
         * finalni runtime adresa (nastavuje ji `_dl_fixup` pred volanim
         * auditu). Pricitat k ni jeste `l_addr` znamena adresu dvakrat
         * posunout - wrapper pak vola smeti a proces padne na SIGSEGV. */
        void *real = (void *)sym->st_value;
        if (!*g_tab[i].real)
            *g_tab[i].real = real;
        else if (*g_tab[i].real != real)
            return sym->st_value;       /* dve definice - nehrajeme si s tim */
        return (uintptr_t)g_tab[i].wrap;
    }
    return sym->st_value;
}
