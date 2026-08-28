/* f2_shim.c — F2 path-translation shim (fakechroot-style) pro elf_loader.
 *
 * Kompilovat proti GLIBC (parrot):  gcc -shared -fPIC -O2 -o f2_shim.so f2_shim.c
 * Nahraje se pres LD_PRELOAD (nastavuje loader v --shim rezimu). Prepisuje
 * open/openat/stat/lstat/access/fopen/opendir/dlopen/chdir/realpath/readlink/
 * execve/... tak, aby absolutni cesty "/" -> "$ROOTFS/". Cesty v exclude listu
 * (/proc /dev /sys /system /data ...) se neprekladaji (realny Android fs).
 * Deti zdedi LD_PRELOAD, takze preklad se siri automaticky.
 *
 * Realnou funkci beru pres dlsym(RTLD_NEXT, jmeno) = glibc definice (za timto
 * .so v poradi nacteni). To funguje i pro glibc-interni volani (fopen->open),
 * na coz GOT-override v own-load flowu nestaci.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <sys/ucontext.h>

/* dlsym vraci void*; explicitni cast na cilovy typ (vyhneme se variadic
 * __typeof__ nesouhlasu u open/openat atd.) */
#define REAL(fn) (void *)dlsym(RTLD_NEXT, #fn)

static const char *g_root = NULL;
static int g_inited = 0;

static void shim_init(void) {
    if (!g_inited) { g_root = getenv("ROOTFS"); g_inited = 1; }
}

static int shim_excluded(const char *p) {
    static const char *excl[] = {
        "/proc", "/dev", "/sys", "/system", "/data", "/apex", "/linkerconfig",
        "/metadata", "/sdcard", "/storage", "/vendor", "/odm", "/product",
        "/persist", "/cache", "/config", "/debug_ramdisk", NULL
    };
    for (int i = 0; excl[i]; i++) {
        size_t l = strlen(excl[i]);
        if (strncmp(p, excl[i], l) == 0 && (p[l] == '/' || p[l] == 0))
            return 1;
    }
    return 0;
}

/* 1 = prelozeno (o naplneno), 0 = ponechat */
static int shim_tr(const char *p, char *o, size_t n) {
    if (!p || p[0] != '/') return 0;
    if (!g_root || !g_root[0]) return 0;
    size_t rl = strlen(g_root);
    if (strncmp(p, g_root, rl) == 0 && (p[rl] == '/' || p[rl] == 0)) return 0;
    if (shim_excluded(p)) return 0;
    snprintf(o, n, "%s%s", g_root, p);
    return 1;
}

int open(const char *p, int f, ...) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    va_list ap; va_start(ap, f); mode_t m = va_arg(ap, mode_t); va_end(ap);
    int (*r)(const char *, int, mode_t) = (int (*)(const char *, int, mode_t))REAL(open);
    return r ? r(pp, f, m) : -1;
}
int open64(const char *p, int f, ...) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    va_list ap; va_start(ap, f); mode_t m = va_arg(ap, mode_t); va_end(ap);
    int (*r)(const char *, int, mode_t) = (int (*)(const char *, int, mode_t))REAL(open64);
    return r ? r(pp, f, m) : -1;
}
int __open(const char *p, int f, ...) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    va_list ap; va_start(ap, f); mode_t m = va_arg(ap, mode_t); va_end(ap);
    int (*r)(const char *, int, mode_t) = (int (*)(const char *, int, mode_t))REAL(__open);
    return r ? r(pp, f, m) : -1;
}
int __open64(const char *p, int f, ...) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    va_list ap; va_start(ap, f); mode_t m = va_arg(ap, mode_t); va_end(ap);
    int (*r)(const char *, int, mode_t) = (int (*)(const char *, int, mode_t))REAL(__open64);
    return r ? r(pp, f, m) : -1;
}
int openat(int d, const char *p, int f, ...) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (d == AT_FDCWD && p && p[0] == '/') { if (shim_tr(p, b, sizeof b)) pp = b; }
    va_list ap; va_start(ap, f); mode_t m = va_arg(ap, mode_t); va_end(ap);
    int (*r)(int, const char *, int, mode_t) = (int (*)(int, const char *, int, mode_t))REAL(openat);
    return r ? r(d, pp, f, m) : -1;
}
int openat64(int d, const char *p, int f, ...) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (d == AT_FDCWD && p && p[0] == '/') { if (shim_tr(p, b, sizeof b)) pp = b; }
    va_list ap; va_start(ap, f); mode_t m = va_arg(ap, mode_t); va_end(ap);
    int (*r)(int, const char *, int, mode_t) = (int (*)(int, const char *, int, mode_t))REAL(openat64);
    return r ? r(d, pp, f, m) : -1;
}
int __openat(int d, const char *p, int f, ...) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (d == AT_FDCWD && p && p[0] == '/') { if (shim_tr(p, b, sizeof b)) pp = b; }
    va_list ap; va_start(ap, f); mode_t m = va_arg(ap, mode_t); va_end(ap);
    int (*r)(int, const char *, int, mode_t) = (int (*)(int, const char *, int, mode_t))REAL(__openat);
    return r ? r(d, pp, f, m) : -1;
}
int __openat64(int d, const char *p, int f, ...) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (d == AT_FDCWD && p && p[0] == '/') { if (shim_tr(p, b, sizeof b)) pp = b; }
    va_list ap; va_start(ap, f); mode_t m = va_arg(ap, mode_t); va_end(ap);
    int (*r)(int, const char *, int, mode_t) = (int (*)(int, const char *, int, mode_t))REAL(__openat64);
    return r ? r(d, pp, f, m) : -1;
}

int stat(const char *p, struct stat *st) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    int (*r)(const char *, struct stat *) = (int (*)(const char *, struct stat *))REAL(stat);
    return r ? r(pp, st) : -1;
}
int stat64(const char *p, struct stat64 *st) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    int (*r)(const char *, struct stat64 *) = (int (*)(const char *, struct stat64 *))REAL(stat64);
    return r ? r(pp, st) : -1;
}
int __xstat(int v, const char *p, struct stat *st) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    int (*r)(int, const char *, struct stat *) = (int (*)(int, const char *, struct stat *))REAL(__xstat);
    return r ? r(v, pp, st) : -1;
}
int __lxstat(int v, const char *p, struct stat *st) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    int (*r)(int, const char *, struct stat *) = (int (*)(int, const char *, struct stat *))REAL(__lxstat);
    return r ? r(v, pp, st) : -1;
}
int lstat(const char *p, struct stat *st) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    int (*r)(const char *, struct stat *) = (int (*)(const char *, struct stat *))REAL(lstat);
    return r ? r(pp, st) : -1;
}
int access(const char *p, int m) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    int (*r)(const char *, int) = (int (*)(const char *, int))REAL(access);
    return r ? r(pp, m) : -1;
}
int faccessat(int d, const char *p, int m, int f) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (d == AT_FDCWD && p && p[0] == '/') { if (shim_tr(p, b, sizeof b)) pp = b; }
    int (*r)(int, const char *, int, int) = (int (*)(int, const char *, int, int))REAL(faccessat);
    return r ? r(d, pp, m, f) : -1;
}

FILE *fopen(const char *p, const char *mode) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    FILE *(*r)(const char *, const char *) = (FILE *(*)(const char *, const char *))REAL(fopen);
    return r ? r(pp, mode) : NULL;
}
FILE *fopen64(const char *p, const char *mode) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    FILE *(*r)(const char *, const char *) = (FILE *(*)(const char *, const char *))REAL(fopen64);
    return r ? r(pp, mode) : NULL;
}

DIR *opendir(const char *p) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    DIR *(*r)(const char *) = (DIR *(*)(const char *))REAL(opendir);
    return r ? r(pp) : NULL;
}

int execve(const char *p, char *const av[], char *const env[]) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    /* deti zdedi LD_PRELOAD (nastaveno v env) => preklad se siri zdarma */
    int (*r)(const char *, char *const[], char *const[]) =
        (int (*)(const char *, char *const[], char *const[]))REAL(execve);
    return r ? r(pp, av, env) : -1;
}
int execv(const char *p, char *const av[]) {
    return execve(p, av, environ);
}
int execvp(const char *p, char *const av[]) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    int (*r)(const char *, char *const[]) = (int (*)(const char *, char *const[]))REAL(execvp);
    return r ? r(pp, av) : -1;
}

void *dlopen(const char *p, int f) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    void *(*r)(const char *, int) = (void *(*)(const char *, int))REAL(dlopen);
    return r ? r(pp, f) : NULL;
}
void *dlopen64(const char *p, int f) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    void *(*r)(const char *, int) = (void *(*)(const char *, int))REAL(dlopen64);
    return r ? r(pp, f) : NULL;
}

int chdir(const char *p) {
    shim_init(); char b[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, b, sizeof b)) pp = b;
    int (*r)(const char *) = (int (*)(const char *))REAL(chdir);
    return r ? r(pp) : -1;
}

char *realpath(const char *p, char *b) {
    shim_init(); char x[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, x, sizeof x)) pp = x;
    char *(*r)(const char *, char *) = (char *(*)(const char *, char *))REAL(realpath);
    return r ? r(pp, b) : NULL;
}

ssize_t readlink(const char *p, char *b, size_t n) {
    shim_init(); char x[PATH_MAX]; const char *pp = p;
    if (shim_tr(p, x, sizeof x)) pp = x;
    ssize_t (*r)(const char *, char *, size_t) = (ssize_t (*)(const char *, char *, size_t))REAL(readlink);
    return r ? r(pp, b, n) : -1;
}
ssize_t readlinkat(int d, const char *p, char *b, size_t n) {
    shim_init(); char x[PATH_MAX]; const char *pp = p;
    if (d == AT_FDCWD && p && p[0] == '/') { if (shim_tr(p, x, sizeof x)) pp = x; }
    ssize_t (*r)(int, const char *, char *, size_t) = (ssize_t (*)(int, const char *, char *, size_t))REAL(readlinkat);
    return r ? r(d, pp, b, n) : -1;
}

/* ===== SIGSYS emulace (Android seccomp blokuje nove syscally) =====
 * Stejna logika jako v elf_loaderu: handler se spusti POUZE pri SIGSYS
 * (syscall zablokovany seccomem), emulujeme benigni hodnotu a preskocime
 * svc #0. Bezi v glibc procesu (loader je pres execve pryc), takze F2 zustava
 * in-process, bez ptrace. */
static void sigsys_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig;
    ucontext_t *ctx = (ucontext_t *)uc;
    long nr = si->si_syscall;
    /* DEBUG: vzdy vypis cislo odmitaneho syscallu */
    {
        char db[80]; char *q = db;
        const char *p = "[SIGSYS-handler] nr=";
        for (; *p; *q++ = *p++);
        char t[24]; int ti = 0; long x = nr;
        if (x == 0) t[ti++] = '0';
        while (x > 0) { t[ti++] = (char)('0' + (x % 10)); x /= 10; }
        while (ti > 0) *q++ = t[--ti];
        *q++ = '\n';
        write(2, db, (size_t)(q - db));
    }
    long emu = -999;
    switch (nr) {
        case 151: emu = getuid(); break;            /* setfsuid */
        case 152: emu = getuid(); break;            /* setfsgid */
        case 140: emu = 0; break;                   /* setpriority */
        case 141: emu = 0; break;                   /* getpriority */
        case 235: emu = 0; break;                   /* mbind */
        case 237: emu = 0; break;                   /* set_mempolicy */
        case 238: emu = 0; break;                   /* migrate_pages */
        case 239: emu = 0; break;                   /* move_pages */
        case 217: case 218: case 219: case 236:     /* add_key/request_key/keyctl/get_mempolicy */
        case 116: case 264: case 439:               /* syslog/name_to_handle_at/faccessat2 */
            emu = -38; break;                       /* -ENOSYS */
        case 180 ... 185:                           /* mq_* */
        case 186 ... 189:                           /* msg* (SysV) */
        case 190 ... 193:                           /* sem* (SysV) */
        case 194: case 195: case 198: case 199:     /* shm* (SysV) */
            emu = -38; break;
        default: break;
    }
    if (emu != -999) {
        ctx->uc_mcontext.pc += 4;            /* preskoc svc #0 */
        ctx->uc_mcontext.regs[0] = emu;      /* emulovana navratova hodnota */
        return;                              /* sigreturn -> pokracovani */
    }
    char buf[64];
    const char *pre = "[SIGSYS] denied syscall nr=";
    char *pp = buf;
    for (const char *q = pre; *q; q++) *pp++ = *q;
    char tmp[24]; int ti = 0; long n = nr;
    if (n == 0) tmp[ti++] = '0';
    while (n > 0) { tmp[ti++] = (char)('0' + (n % 10)); n /= 10; }
    while (ti > 0) *pp++ = tmp[--ti];
    *pp++ = '\n';
    write(2, buf, (size_t)(pp - buf));
    _exit(159);
}

__attribute__((constructor)) static void shim_ctor(void) {
    static char altstack[32768];
    stack_t ss; memset(&ss, 0, sizeof ss);
    ss.ss_sp = altstack; ss.ss_size = sizeof altstack;
    sigaltstack(&ss, NULL);
    { char m[32]; const char *p="SHIM_CTOR_RUN\n"; const char *q=p; char *r=m;
      for(;*q;*r++=*q++); write(2, m, (size_t)(r-m)); }
    struct sigaction sc; memset(&sc, 0, sizeof sc);
    sc.sa_sigaction = sigsys_handler;
    sc.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int rv = sigaction(SIGSYS, &sc, NULL);
    { char m[48]; char *r=m; const char *p="SHIM_SIGACT_RV="; const char *q=p;
      for(;*q;*r++=*q++); char t[24]; int ti=0; long x=rv;
      if(x==0)t[ti++]='0'; while(x>0){t[ti++]=(char)('0'+(x%10));x/=10;}
      while(ti>0)*r++=t[--ti]; *r++='\n'; write(2,m,(size_t)(r-m)); }
}
