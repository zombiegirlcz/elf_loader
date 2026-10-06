#define _GNU_SOURCE 1
#define ELF_LOADER_VERSION "0.1-dev"
#include <stdio.h>
#include <signal.h>
#include "../include/elf_loader.h"

static void print_help(const char *prog) {
    fprintf(stdout,
        "elf_loader - own-loading glibc launcher for Android (aarch64)\n"
        "\n"
        "Usage:\n"
        "  %s --help | -h\n"
        "  %s --version | -V\n"
        "  %s --check <file>\n"
        "  %s [--lazy] --run <elf> [args..]\n"
        "  %s [--lazy] --own <elf> <shared.so> [args..]\n"
        "  %s [--lazy] --ownall <elf> [args..]\n"
        "  %s [--lazy] --shim <elf> [args..]\n"
        "  %s init zsh|bash\n"
        "  %s <elf>                        (introspect)\n"
        "\n"
        "Modes:\n"
        "  --run         host-loader mode: execute ELF with host libc\n"
        "  --own         own-load one shared module into a private scope\n"
        "  --ownall      own-load all distro deps + guest binary (parrot glibc)\n"
        "  --shim        F2 path-translation shim for chroot-less guest paths\n"
        "  init zsh|bash print `eval`-able env defaults (LOCPATH/LC_ALL) + helpers\n"
        "  <elf>         introspect base/entry/symbols without execution\n"
        "\n"
        "Options:\n"
        "  --lazy        lazy PLT binding (faster startup, more traps)\n"
        "  --help/-h     this help\n"
        "  --version/-V  print loader version\n"
        "  --check <f>   validate ELF file and print base/entry/deps\n"
        "\n"
        "Environment:\n"
        "  ROOTFS        guest rootfs for --ownall/--shim\n"
        "  ELF_LOADER    loader binary path (default /proc/self/exe)\n"
        "  ELF_DEBUG     enable debug trace\n"
        "  F2_FILTER     enable seccomp path filter in --shim mode\n"
        "\n"
        "Examples:\n"
        "  %s --check /data/.../parrot/bin/ls\n"
        "  %s --run /data/.../parrot/bin/true\n"
        "  ROOTFS=/data/.../parrot %s --ownall /data/.../parrot/bin/ls -la /etc\n"
        "  ROOTFS=/data/.../parrot %s --shim /data/.../parrot/usr/bin/awk 'BEGIN{print 1+2}'\n"
        "  %s /data/.../parrot/bin/cat /etc/hostname\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog,
        prog, prog, prog, prog, prog);
}


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <malloc.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
extern int elf_debug(void);
void elf_set_crash_scope(elf_scope_t *s);

static const char *status_str(sym_status_t st) {
    switch (st) {
    case SYM_DEFINED:
        return "defined";
    case SYM_IMPORT:
        return "import";
    default:
        return "not found";
    }
}

static void introspect(const char *path) {
    elf_object_t *obj = elf_load(path);
    if (!obj) {
        fprintf(stderr, "[-] Failed to load ELF\n");
        return;
    }

    printf("[+] Base:  %p\n", obj->base_addr);
    printf("[+] Entry: %p\n", obj->entry_point);
    printf("[+] Size:  %zu bytes\n", obj->total_size);
    printf("[+] .symtab: %zu symbols, .dynsym: %zu symbols, deps: %zu\n",
           obj->symtab_count, obj->dynsym_count, obj->handle_count);

    const char *test_syms[] = {"main", "printf", "puts", "__libc_start_main"};
    for (size_t i = 0; i < sizeof(test_syms) / sizeof(test_syms[0]); i++) {
        void *addr = NULL;
        sym_status_t st = elf_resolve_symbol(obj, test_syms[i], &addr);
        if (st == SYM_NOT_FOUND)
            printf("[-] '%s' -> %s\n", test_syms[i], status_str(st));
        else
            printf("[+] '%s' -> %s @ %p\n", test_syms[i], status_str(st), addr);
    }

    elf_relocate(obj);
    elf_unload(obj);
}

static int run(const char *path, int argc, char **argv, char **envp) {
    elf_init_argc = argc;
    elf_init_argv = argv;
    elf_init_envp = envp;
    elf_object_t *obj = elf_load(path);
    if (!obj) {
        fprintf(stderr, "[-] Failed to load ELF\n");
        return 1;
    }

    if (elf_debug())
        printf("[+] Base: %p Entry: %p deps: %zu\n",
           obj->base_addr, obj->entry_point, obj->handle_count);

    g_libc_base = 0;
    g_exe_base = (uintptr_t)obj->base_addr;

    if (!getenv("ELF_LOADER_SKIP_RELOC") && elf_relocate(obj) != 0) {
        fprintf(stderr, "[-] Relocation failed\n");
        elf_unload(obj);
        return 1;
    }

    int ret = elf_run(obj, argc, argv, envp);
    elf_unload(obj);
    return ret;
}

static int run_ownall(const char *path, int argc, char **argv, char **envp);

/* F2 seccomp path-filter je volitelny: pri re-execu (shim_execve) dedi dite
 * filtr, ale SIGSYS handler je po execve SIG_DFL a bionic ld.so ditete dela
 * openat jeste pred main() -> SIGSYS -> pad. Proto je filtr defaultne VYPNUTY
 * v --shim rezimu; preklad cest zajistuji PLT override + inline hooky +
 * explicitni reseni symlinku v elf_load. F2_FILTER=1 filtr zapne (bez re-execu). */
static int f2_should_filter(void) {
    /* F2 seccomp filtr je DEFAULTNE VYPNUTY. Filtr se dedi pres execve do
     * fork+exec deti (subprocess, zsh -i compinit/fzf), kde mezi execve a
     * instalaci SIGSYS handleru bezici bionicky ld.so udela openat -> TRAP
     * bez handleru -> dite umre ("Bad system call").
     * DNS a interni cesty resi inline-hook __open64_nocancel (viz vyse).
     * Zapnout lze F2_FILTER=1 (experimenty). */
    const char *v = getenv("F2_FILTER");
    return (v && v[0] && v[0] != '0');
}

/* F2: path-translatni seccomp (non-root) je implementovan v elf_loader.c
 * (install_f2_path_filter + sigsys_handler): pro path-syscally (openat/statx/
 * newfstatat/readlinkat/faccessat) vraci SIGSYS, handler prelozi cestu a
 * zemuluje syscall pomoci SENTINEL v x5 (filtr jej pusti, zabrani zacykleni).
 * Stejna sada plati i pro Androidem blokovane emulovatelne syscally
 * (setfsuid/keyctl/... viz sigsys_handler). */

/* ===== F2: path-translation shim (non-root, fakechroot-style) =====
 * Prepend $ROOTFS k absolutnim cestam ("/" -> "$ROOTFS/") pro parrot binarky
 * bezici in-process (parrot glibc). Skutecna funkce se vola pres
 * elf_scope_lookup(elf_own_scope, name) -> glibc symbol ve vlastnim scopu
 * (ne dlsym, to by dalo bionickou). execve navic re-execuje loader, aby i
 * child procesy (starship/gh/...) bežely pod loaderem s parrot glibc + shimem.
 * POZOR: cesty jako /proc /dev /sys /system /data ... se neprekladaji (realny
 * Android fs). */
static const char *g_shim_root = NULL;
static const char *g_shim_loader = NULL;
static const char *g_exec_mode = "--ownall";
static int g_f2_active = 0;  /* 1 = F2 rezim (--shim), povol inline-hooky */
static elf_scope_t *g_shim_scope = NULL;  /* platny scope behem F2 behu */

/* Wrap: log any guest sigaction call for SIGSYS (31) to diag.txt,
 * so we can detect if starship/glibc resets our handler. */
#define ELF_SENTINEL 0x1234567890ABCDEFULL
static void raw_wr2(const char *b, int n);

/* Per-PID diag cesta (stejne jako v elf_loader.c) - vnorene loadery
 * nesmi michat jeden sdileny diag.txt. */
static char g_diag_path_main[160];
static const char *diag_path_main(void) {
    if (g_diag_path_main[0])
        return g_diag_path_main;
    register long x8 __asm__("x8") = 172; /* getpid */
    register long x0 __asm__("x0") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    long pid = x0;
    const char *pre = "/data/user/0/com.linux_core/files/usr/diag.";
    int i = 0;
    while (pre[i]) { g_diag_path_main[i] = pre[i]; i++; }
    char t[24]; int ti = 0;
    if (pid <= 0) pid = 1;
    while (pid > 0) { t[ti++] = (char)('0' + (pid % 10)); pid /= 10; }
    while (ti > 0) g_diag_path_main[i++] = t[--ti];
    const char *suf = ".txt";
    for (int k = 0; suf[k]; k++) g_diag_path_main[i++] = suf[k];
    g_diag_path_main[i] = 0;
    return g_diag_path_main;
}
static long my_raw_syscall6(long n, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5) : "memory","cc");
    return x0;
}
static int (*diag_real_sigaction)(int, const struct sigaction *, struct sigaction *) = NULL;
static int diag_wrapped_sigaction(int signum, const struct sigaction *act,
                                  struct sigaction *oldact) {
    /* TRACE vsech instalaci fatalnich handleru: kdo je instaluje (addr) */
    if (getenv("ELF_LOADER_SIGTRACE") && act &&
        (signum == 11 || signum == 7 || signum == 4 || signum == 6 ||
         signum == 8 || signum == 5 || signum == 31)) {
        char b[96]; int i = 0;
        const char *q = "SA sig=";
        while (*q) b[i++] = *q++;
        int v = signum;
        if (v >= 10) b[i++] = (char)('0' + v / 10);
        b[i++] = (char)('0' + v % 10);
        q = " h="; while (*q) b[i++] = *q++;
        unsigned long u = (unsigned long)(act->sa_sigaction);
        static const char hxd[] = "0123456789abcdef";
        for (int sh = 60; sh >= 0; sh -= 4) b[i++] = hxd[(u >> sh) & 0xf];
        q = " f=0x"; while (*q) b[i++] = *q++;
        /* guest je glibc (152B struct sigaction, sa_flags @136); nase
         * (bionic) act->sa_flags by cetlo offset 16 -> nesmysl. */
        u = *(const unsigned long *)((const unsigned char *)act + 136);
        for (int sh = 28; sh >= 0; sh -= 4) b[i++] = hxd[(u >> sh) & 0xf];
        b[i++] = 10;
        raw_wr2(b, i);
    }
    if (signum == SIGSYS && getenv("ELF_LOADER_SIGTRACE")) {
        long fd = my_raw_syscall6(56, -100,
                              (long)(unsigned long)diag_path_main(),
                              0x441L, 0644L, 0, ELF_SENTINEL);
        if (fd >= 0) {
            const char msg[] = "SIGACTION-SIGSYS\n";
            my_raw_syscall6(64, fd, (long)(unsigned long)msg, sizeof(msg)-1, 0, 0, ELF_SENTINEL);
            my_raw_syscall6(57, fd, 0, 0, 0, 0, ELF_SENTINEL);
        }
    }
    if (!diag_real_sigaction)
        diag_real_sigaction = (int (*)(int, const struct sigaction *, struct sigaction *))elf_scope_lookup(g_shim_scope, "sigaction");
    /* Fatalni signaly: uloz guest handler a NENECH nas fault_handler
     * prepsat. Guest handler pak chainujeme z fault_handleru. */
    if (act && (signum == 11 || signum == 7 || signum == 4 ||
                signum == 6 || signum == 8 || signum == 5)) {
        elf_set_guest_fatal(signum, act);
        return 0;
    }
    /* SIGSYS: loader pres seccomp TRAP emuluje/preklada syscally (faccessat,
     * openat, ...) ve svem sigsys_handleru. Interaktivni bash si pri startu
     * instaluje vlastni handler pro vsechny "terminating" signaly vcetne
     * SIGSYS -> dalsi TRAP skoncil v bashovem handleru a proces umrel na
     * signal 31 (tmux panel s login bashem). Guest handler jen ulozime. */
    if (act && signum == 31) {
        elf_set_guest_fatal(signum, act);
        return 0;
    }
    return diag_real_sigaction ? diag_real_sigaction(signum, act, oldact) : -1;
}
/* Rucni string copy bez bionic libc (v parrot TLS kontextu by strncmp/snprintf
 * deref. bionic errno/TLS a crashl). */
static size_t shim_strlen(const char *s) {
    size_t l = 0; while (s[l]) l++; return l;
}
static int shim_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
static void shim_memcpy(void *d, const void *s, size_t n) {
    char *dd = (char *)d; const char *ss = (const char *)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void shim_strcpy(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    if (!dstsz) return;
    while (src[i] && i < dstsz - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void shim_strcat(char *dst, size_t dstsz, const char *src) {
    size_t dl = shim_strlen(dst);
    if (dl >= dstsz) return;
    shim_strcpy(dst + dl, dstsz - dl, src);
}
static char *shim_strchr(const char *s, char c) {
    while (*s) { if (*s == c) return (char *)s; s++; }
    return c == 0 ? (char *)s : NULL;
}
static int shim_strcmp(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (unsigned char)a[i] - (unsigned char)b[i];
}
/* Raw aarch64 syscall — no bionic errno/TLS access, safe under parrot TP. */
static long shim_raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}
/* Guest environ lookup: prefer glibc's environ (reflects guest setenv calls),
 * fall back to bionic environ. Reading a global pointer is TLS-safe. */
static char **guest_environ(void) {
    if (g_shim_scope) {
        char ***ep = (char ***)elf_scope_lookup(g_shim_scope, "environ");
        if (ep && *ep) return *ep;
    }
    return environ;
}
static int shim_excluded(const char *p) {
    static const char *excl[] = {
        "/proc", "/dev", "/sys", "/system", "/data", "/apex", "/linkerconfig",
        "/metadata", "/sdcard", "/storage", "/vendor", "/odm", "/product",
        "/persist", "/cache", "/config", "/debug_ramdisk", NULL
    };
    for (int i = 0; excl[i]; i++) {
        size_t l = shim_strlen(excl[i]);
        if (shim_strncmp(p, excl[i], l) == 0 && (p[l] == '/' || p[l] == 0))
            return 1;
    }
    return 0;
}

/* 1 = prelozeno (out naplneno), 0 = ponechat */
static int shim_translate(const char *p, char *out, size_t n) {
    if (!p || p[0] != '/') return 0;
    if (!g_shim_root || !g_shim_root[0]) return 0;
    size_t rl = shim_strlen(g_shim_root);
    if (shim_strncmp(p, g_shim_root, rl) == 0 && (p[rl] == '/' || p[rl] == 0)) return 0;
    if (shim_excluded(p)) return 0;
    size_t pl = shim_strlen(p);
    if (rl + pl + 1 > n) return 0;            /* out neni zkraceno -> bezpecne */
    shim_memcpy(out, g_shim_root, rl);
    shim_memcpy(out + rl, p, pl + 1);
    return 1;
}

/* ===== F2 inline-hook infra =====
 * Patchneme glibc leaf funkce (open/openat/stat/...) primo v kodu. To zachyti
 * i glibc-interni volani (opendir->openat64 je raw syscall, ktery PLT-override
 * nechyta). Loader mapuje glibc jako anonymni RW->RX, takze mprotect zpet na
 * RWX pro patch kodu na Androidu projde (zadny file-backed W^X). */
static void *g_orig_open = NULL, *g_orig_open64 = NULL, *g_orig_openat = NULL,
            *g_orig_openat64 = NULL;
static void *g_orig_open64_nocancel = NULL;  /* interni glibc open pro DNS/NSS */
static void *g_orig_stat = NULL, *g_orig_stat64 = NULL, *g_orig___xstat = NULL,
            *g_orig_lstat = NULL, *g_orig___lxstat = NULL;
static void *g_orig_lstat64 = NULL, *g_orig_fstat64 = NULL, *g_orig_fstatat64 = NULL,
            *g_orig_statvfs = NULL, *g_orig_statvfs64 = NULL;
static void *g_orig_access = NULL, *g_orig_euidaccess = NULL, *g_orig_faccessat = NULL;
static void *g_orig_statx = NULL, *g_orig_fstatat = NULL, *g_orig_newfstatat = NULL;
static void *g_orig_symlink = NULL, *g_orig_symlinkat = NULL, *g_orig_link = NULL,
            *g_orig_rename = NULL, *g_orig_renameat = NULL, *g_orig_renameat2 = NULL,
            *g_orig_unlink = NULL, *g_orig_unlinkat = NULL, *g_orig_rmdirat = NULL,
            *g_orig_mkdir = NULL,
            *g_orig_mkdirat = NULL, *g_orig_rmdir = NULL;
static void *g_orig_execve = NULL, *g_orig_execv = NULL, *g_orig_execvp = NULL,
            *g_orig_execvpe = NULL, *g_orig_execveat = NULL,
            *g_orig_execl = NULL, *g_orig_execlp = NULL, *g_orig_execle = NULL;
static void *g_orig_fopen = NULL, *g_orig_fopen64 = NULL,
            *g_orig___xstat64 = NULL, *g_orig___lxstat64 = NULL,
            *g_orig___fxstatat = NULL, *g_orig___fxstatat64 = NULL,
            *g_orig_faccessat2 = NULL,
            *g_orig_getrlimit = NULL, *g_orig_prlimit64 = NULL;
static void *g_orig_fileno_unlocked = NULL;
static void *g_orig_fileno = NULL;
static void *g_orig_flockfile = NULL;
static void *g_orig_close = NULL;
static void *g_orig_setfsuid = NULL, *g_orig_setfsgid = NULL;
static void *g_orig_mprotect = NULL;
/* ELF_LOADER_VMTRACE=1 -> loguj kazdy mmap/mprotect/munmap (diagnostika
 * V8 read-only heapu: kdo udelal stranku r--p pred fatalnim store). */
static int g_vmtrace = 0;
/* ELF_LOADER_RO_KEEP_WRITE=1 -> mprotect, ktery odebira PROT_WRITE z datove
 * stranky, necha zapis povoleny. Diagnostika/workaround pro V8 seal
 * read-only heapu: Builtins_InterpreterEntryTrampoline zapisuje age=0 do
 * SharedFunctionInfo, ktery po sealu lezi v r--p strance -> SIGSEGV. */
static int g_ro_keep_write = 0;
static void *g_orig_opendir = NULL, *g_orig_readlink = NULL, *g_orig_readlinkat = NULL,
            *g_orig_realpath = NULL, *g_orig_dlopen = NULL, *g_orig_chdir = NULL;

typedef struct f2_hook { const char *n; void *shim; void **orig; } f2_hook_t;

/* Najde volnou 4KB stranku do +-120MB od 'addr' (B range je +-128MB)
 * prohledanim mezer v /proc/self/maps. mmap(NULL) by dal stranku GB daleko,
 * mimo dosah vetve. */
static void *alloc_near(void *addr) {
    uintptr_t want = (uintptr_t)addr;
    /* uintptr_t underflow guard: pro `want` < 0x7800000 (napr. non-PIE ET_EXEC
     * nacteny nizko, jako node na 0x400000) by `want - 0x7800000` podteklo na
     * hodnotu blizko UINT64_MAX, cimz by `gs >= mina` NIKDY neprosla zadnou
     * realnou mezerou -> alloc_near vzdy spadne na vzdaleny mmap(NULL,...) a
     * nasledny patch_branch selze (branch mimo dosah). Zjisteno empiricky:
     * ELF_LOADER_TRACE_CALL na adresu uvnitr node binarky (~0xde9854). */
    uintptr_t mina = (want > 0x7800000) ? want - 0x7800000 : 0x10000;
    uintptr_t maxa = want + 0x7800000;
    char line[256];
    uintptr_t prev = 0;
    FILE *mf = fopen("/proc/self/maps", "r");
    if (mf) {
        while (fgets(line, sizeof line, mf)) {
            uintptr_t s = 0, e = 0;
            if (sscanf(line, "%lx-%lx", &s, &e) != 2) continue;
            if (prev && s > prev) {
                uintptr_t gs = (prev + 0xFFF) & ~(uintptr_t)0xFFF;
                if (gs + 0x1000 <= s && gs >= mina && gs + 0x1000 <= maxa) {
                    void *p = mmap((void *)gs, 4096,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                                   -1, 0);
                    if (p != MAP_FAILED) { fclose(mf); return p; }
                }
            }
            prev = e;
        }
        fclose(mf);
    }
    return mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* Zapise na 'target' vetev k 'dst' (B pokud v rozsahu +-128MB, jinak bridge).
 * Vraci 0 ok, -1 chyba. */
static uint32_t branch_insn(void *from, void *to) {
    int64_t off = (int64_t)((uintptr_t)to - (uintptr_t)from);
    if (off >= -0x8000000 && off <= 0x7FFFFFFC)
        return 0x14000000 | ((uint32_t)(off >> 2) & 0x3FFFFFF);
    return 0;
}
static void *make_bridge(void *dst) {
    void *b = alloc_near(dst);
    if (b == MAP_FAILED) return NULL;
    *(uint32_t *)b = 0x58000050;                    /* LDR x16,[PC,#8] */
    *(uint32_t *)((char *)b + 4) = 0xD61F0200;     /* BR x16 */
    *(uint64_t *)((char *)b + 8) = (uint64_t)dst;
    __builtin___clear_cache(b, (char *)b + 16);
    mprotect(b, 4096, PROT_READ | PROT_EXEC);
    return b;
}
static int patch_branch(void *target, void *dst) {
    uintptr_t pg = (uintptr_t)target & ~(uintptr_t)4095;
    /* W^X: pokud mprotect na RWX selze (Android SELinux), nelze glibc kod
     * patchovat -> hook se preskoci (F2 zustane partial, bez crashnuti). */
    if (mprotect((void *)pg, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        if (elf_debug()) fprintf(stderr, "[hook] patch_branch: mprotect RWX FAIL "
                                    "(target=%p) -> W^X blokuje inline-hook\n", target);
        return -1;
    }
    uint32_t bi = branch_insn(target, dst);
    if (bi) {
        *(uint32_t *)target = bi;
    } else {
        /* mimo +-128MB: bridge musi byt blizko targetu (nikoliv dst=shim,
         * ktery je v loaderu ~GB daleko), jinak B target->bridge nestihne. */
        void *b = alloc_near(target);
        if (b == MAP_FAILED) { mprotect((void *)pg, 4096, PROT_READ | PROT_EXEC); return -1; }
        *(uint32_t *)b = 0x58000050;                    /* LDR x16,[PC,#8] */
        *(uint32_t *)((char *)b + 4) = 0xD61F0200;     /* BR x16 */
        *(uint64_t *)((char *)b + 8) = (uint64_t)dst;
        __builtin___clear_cache(b, (char *)b + 16);
        /* KRITICKE: alloc_near vraci RW (ne X) stranku. Bez tohoto mprotect
         * skoci B na bridge a spadne na "exec neexecutabilni stranky"
         * (fault: pc == ad == adresa bridge). make_bridge() to dela, tady
         * to chybelo. */
        mprotect(b, 4096, PROT_READ | PROT_EXEC);
        uint32_t b2 = branch_insn(target, b);
        if (!b2) { mprotect((void *)pg, 4096, PROT_READ | PROT_EXEC); return -1; }
        *(uint32_t *)target = b2;
    }
    __builtin___clear_cache(target, (char *)target + 4);
    mprotect((void *)pg, 4096, PROT_READ | PROT_EXEC);
    return 0;
}
/* ELF_LOADER_TRACE_CALL=<hex_addr>: genericky "call-site" tracer. Patchuje
 * PRESNE JEDNU instrukci `blr xN` na danem miste (napr. volani Builtins_JSEntry
 * z v8::internal::Invoke) tak, aby se pred jejim provedenim zalogovaly
 * registry x0-x17, a pak se instrukce provede BEZE ZMENY (trampolina).
 * Cil: zjistit, zda jsou argumenty predavane do V8 JSEntry (x0-x5 dle V8 ABI,
 * x8 = cilova adresa) spravne UZ NA VSTUPU do generovaneho kodu, nebo je
 * korupce/spatna hodnota pritomna uz na C++ strane (Invoke/Execution::Call)
 * pred timto volanim. `blr xN` je jedina instrukce bez PC-relativni zavislosti,
 * takze ji lze bezpecne zkopirovat do trampoliny beze zmeny.
 *
 * POZOR TP: shim se instaluje pod LOADER TP (run_ownall, pred elf_run), ale
 * SPOUSTI se az PO prepnuti na guest/parrot TP (blr x8 je uvnitr node/V8
 * kodu). Logger proto NESMI volat fprintf/libc stdio - ty ctou bionicky
 * stack-guard/errno/FILE* pres TP, ktery uz ukazuje do parrot TLS layoutu
 * (divoky pointer -> SIGSEGV BEZ jakehokoliv vystupu, i mimo nas fault
 * handler). Log jde vyhradne raw syscallem (shim_raw_syscall6), presne jako
 * shim_mmap_log/shim_mprotect o par set radku niz. */
static void shim_hex(char **pp, unsigned long v, int nib);
static long shim_raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
static void trace_call_logger(unsigned long *regs) {
    /* Marker NEZAVISLY na 'regs' - overuje jestli sem vubec dorazime,
     * bez ohledu na to, jestli je pointer/stack v poradku. */
    {
        long fd0 = shim_raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL,
            (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/trace_call.txt",
            0x441L, 0644L, 0, 0);
        if (fd0 >= 0) {
            const char m0[] = "TC-ENTER\n";
            shim_raw_syscall6(64, fd0, (long)(unsigned long)m0, sizeof(m0) - 1, 0, 0, 0);
            shim_raw_syscall6(57, fd0, 0, 0, 0, 0, 0);
        }
    }
    /* fd 2 muze byt v tuto chvili (runtime, ne load-time) prepsany/zavreny
     * node/libuv vlastni stdio inicializaci - zapis radeji do vlastniho
     * pevneho souboru (stejny vzor jako diag.txt zapisy jinde v projektu),
     * O_APPEND, nezavisle na aktualnim stavu fd 0/1/2. */
    char b[300]; char *i = b;
    const char *p = "[TRACE_CALL] x0="; while (*p) *i++ = *p++;
    shim_hex(&i, regs[0], 16);
    p = " x1="; while (*p) *i++ = *p++; shim_hex(&i, regs[1], 16);
    p = " x2="; while (*p) *i++ = *p++; shim_hex(&i, regs[2], 16);
    p = " x3="; while (*p) *i++ = *p++; shim_hex(&i, regs[3], 16);
    p = "\n             x4="; while (*p) *i++ = *p++; shim_hex(&i, regs[4], 16);
    p = " x5="; while (*p) *i++ = *p++; shim_hex(&i, regs[5], 16);
    p = " x6="; while (*p) *i++ = *p++; shim_hex(&i, regs[6], 16);
    p = " x7="; while (*p) *i++ = *p++; shim_hex(&i, regs[7], 16);
    p = "\n             x8="; while (*p) *i++ = *p++; shim_hex(&i, regs[8], 16);
    p = " x9="; while (*p) *i++ = *p++; shim_hex(&i, regs[9], 16);
    p = " x10="; while (*p) *i++ = *p++; shim_hex(&i, regs[10], 16);
    p = " x11="; while (*p) *i++ = *p++; shim_hex(&i, regs[11], 16);
    *i++ = '\n';
    long fd = shim_raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL,
                                 (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/trace_call.txt",
                                 0x441L /*O_WRONLY|O_CREAT|O_APPEND*/, 0644L, 0, 0);
    if (fd >= 0) {
        shim_raw_syscall6(64, fd, (long)b, (long)(i - b), 0, 0, 0);
        shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
    }
    /* jeste zkusit fd 2 (nekdy funguje, malo stoji navic) */
    shim_raw_syscall6(64, 2, (long)b, (long)(i - b), 0, 0, 0);
}
/* ELF_LOADER_PAUSE_CALL=<hex_addr>: jako TRACE_CALL, ale navic uspi 30s
 * (SYS_nanosleep) pred provedenim puvodni BLR instrukce. Na rozdil od
 * PAUSE_ENTRY (hookuje uvnitr InterpreterEntryTrampoline, ktera se pod
 * --inspect-brk chova jinak/nedosahne se stejne) cili na CALL SITE
 * `v8::internal::Invoke` -> JSEntryTrampoline (0xde9854) - prvni skok do
 * V8 JS vubec, PRED bootstrap skripty, nezavisly na interpreter dispatch
 * detailech. */
static void trace_call_pause_logger(unsigned long *regs) {
    trace_call_logger(regs);
    struct { long tv_sec; long tv_nsec; } ts = { 30, 0 };
    shim_raw_syscall6(101 /* nanosleep */, (long)(unsigned long)&ts, 0, 0, 0, 0, 0);
}
static uint32_t tc_enc_str(int rt, int rn, int off) {
    return 0xF9000000u | (((uint32_t)(off / 8)) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t tc_enc_ldr(int rt, int rn, int off) {
    return 0xF9400000u | (((uint32_t)(off / 8)) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
/* ELF_LOADER_TRACE_RING=<hex_addr>: pro HOT-PATH mista (napr. interpreter
 * dispatch loop, volana na KAZDY bytecode - stovky tisic za beh) je
 * souborovy/syscallovy logger prilis pomaly a zaplavi log. ring_logger misto
 * toho jen ulozi x0-x2 do staticke kruhove pameti (zadny syscall v hot
 * path) - obsah se vypise az z fault_handleru pri padu (elf_dispatch_ring_dump
 * v elf_loader.c), takze vidime POSLEDNICH N volani PRED SIGSEGV. */
#define DISPATCH_RING_N 16
unsigned long g_dispatch_ring[DISPATCH_RING_N][3];
int g_dispatch_ring_idx;
static void ring_logger(unsigned long *regs) {
    int i = g_dispatch_ring_idx % DISPATCH_RING_N;
    g_dispatch_ring[i][0] = regs[0];
    g_dispatch_ring[i][1] = regs[1];
    g_dispatch_ring[i][2] = regs[2];
    g_dispatch_ring_idx++;
}

static void install_regs_trace_impl(void *target, void *logger_fn, const char *tag,
                                    int require_blr);
static void install_call_trace_with(void *target, void *logger_fn, const char *tag) {
    install_regs_trace_impl(target, logger_fn, tag, 1);
}
/* require_blr=0: libovolna instrukce na vstupu funkce (ELF_LOADER_TRACE_REGS).
 * Uklada x0-x17 + x30 (regs[18]), logger cte vsechny argumenty, po navratu
 * se obnovi -> funkce bezi s puvodnimi registry. */
static void install_regs_trace_impl(void *target, void *logger_fn, const char *tag,
                                    int require_blr) {
    uint32_t ins0 = *(const uint32_t *)target;
    /* BLR Xn kontrola: bity [31:10] = 1101011 0 001 11111 000000, Rn v [9:5]. */
    if (require_blr && (ins0 & 0xFFFFFC1Fu) != 0xD63F0000u) {
        fprintf(stderr, "[%s] %p: ins=%08x neni BLR Xn, preskakuji\n",
                tag, target, ins0);
        return;
    }
    /* tramp: [puvodni BLR instrukce beze zmeny][skok zpet na target+4] */
    void *tramp = alloc_near(target);
    if (tramp == MAP_FAILED) { fprintf(stderr, "[%s] alloc_near(tramp) FAIL\n", tag); return; }
    *(uint32_t *)tramp = ins0;
    uint32_t back = branch_insn((char *)tramp + 4, (char *)target + 4);
    if (back) {
        *(uint32_t *)((char *)tramp + 4) = back;
    } else {
        void *b = make_bridge((char *)target + 4);
        if (!b) { fprintf(stderr, "[%s] make_bridge(back) FAIL\n", tag); return; }
        uint32_t b2 = branch_insn((char *)tramp + 4, b);
        if (!b2) { fprintf(stderr, "[%s] tramp->back OOR\n", tag); return; }
        *(uint32_t *)((char *)tramp + 4) = b2;
    }
    __builtin___clear_cache(tramp, (char *)tramp + 8);
    mprotect(tramp, 4096, PROT_READ | PROT_EXEC);

    /* shim: ulozi x0-x17 (18 regs, 144 B) na stack, zavola logger(sp),
     * obnovi x0-x17, skoci na tramp. Zadna PC-relativni zavislost krome
     * literalu (ldr x9,[pc,#-8]), ktery je pred nim - vzdy v dosahu. */
    void *shim = alloc_near((char *)target + 64);
    if (shim == MAP_FAILED) { fprintf(stderr, "[%s] alloc_near(shim) FAIL\n", tag); return; }
    uint32_t *sc = (uint32_t *)shim;
    int i = 0;
    sc[i++] = 0xD10403FFu;                    /* sub sp, sp, #256 (16-align, 144 potreba) */
    for (int r = 0; r <= 17; r++)
        sc[i++] = tc_enc_str(r, 31, r * 8);   /* str xR, [sp, #R*8] */
    sc[i++] = tc_enc_str(30, 31, 18 * 8);     /* str x30, [sp, #144] (regs[18]) */
    sc[i++] = 0x910003E0u;                    /* mov x0, sp (arg logger) */
    /* BUG (opraveno): sekvencni provadeni NEPRESKOCI literal samo od sebe -
     * bez explicitni vetve tu CPU spadne do 8 B literalu jako do instrukci
     * (SIGILL presne na literal bytech, overeno empiricky na zarizeni).
     * Skok o 3 slova (12 B) preskoci literal na LDR. */
    sc[i++] = 0x14000003u;                    /* b +12 (preskoc literal) */
    /* literal (8 B) pred ldr, ldr cte [pc,#-8] */
    uint64_t *lit = (uint64_t *)&sc[i];
    *lit = (uint64_t)(uintptr_t)logger_fn;
    i += 2;
    sc[i++] = 0x58FFFFC9u;                    /* ldr x9, [pc, #-8] -> logger addr */
    sc[i++] = 0xD63F0120u;                    /* blr x9 */
    for (int r = 0; r <= 17; r++)
        sc[i++] = tc_enc_ldr(r, 31, r * 8);   /* ldr xR, [sp, #R*8] */
    sc[i++] = tc_enc_ldr(30, 31, 18 * 8);     /* ldr x30, [sp, #144] */
    sc[i++] = 0x910403FFu;                    /* add sp, sp, #256 */
    uint32_t jb = branch_insn((char *)shim + (size_t)i * 4, tramp);
    if (!jb) { fprintf(stderr, "[%s] shim->tramp OOR\n", tag); return; }
    sc[i++] = jb;
    __builtin___clear_cache(shim, (char *)shim + (size_t)i * 4);
    mprotect(shim, 4096, PROT_READ | PROT_EXEC);

    if (patch_branch(target, shim) == 0) {
        fprintf(stderr, "[%s] installed at %p (tramp=%p shim=%p) logger=%p n_instr=%d\n",
                tag, target, tramp, shim, logger_fn, i);
    } else {
        fprintf(stderr, "[%s] patch_branch FAILED at %p\n", tag, target);
    }
}
/* ELF_LOADER_TRACE_REGS=<hex>[,...]: na vstupu funkce zaloguje x0-x3 + x30
 * (raw zapis, guest TP-safe) do usr/trace_call.txt. */
static void trace_regs_logger(unsigned long *regs) {
    char b[200]; char *i = b;
    const char *p = "[REGS] x0="; while (*p) *i++ = *p++;
    shim_hex(&i, regs[0], 16);
    p = " x1="; while (*p) *i++ = *p++; shim_hex(&i, regs[1], 16);
    p = " x2="; while (*p) *i++ = *p++; shim_hex(&i, regs[2], 16);
    p = " x3="; while (*p) *i++ = *p++; shim_hex(&i, regs[3], 16);
    p = " x30="; while (*p) *i++ = *p++; shim_hex(&i, regs[18], 16);
    *i++ = '\n';
    long fd = shim_raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL,
                                 (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/trace_call.txt",
                                 0x441L, 0644L, 0, 0);
    if (fd >= 0) {
        shim_raw_syscall6(64, fd, (long)b, (long)(i - b), 0, 0, 0);
        shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
    }
}
/* ELF_LOADER_TRACE_PEEK: k [REGS] pridat bezpecne cteni pameti
 * (process_vm_readv) - qwordy na *x0, x2 a builtin_table_ isolate=x1. */
static int peek_q(unsigned long a, unsigned long *out, int n) {
    struct { void *b; unsigned long l; } lv = { out, (unsigned long)n * 8 }, rv = { (void *)a, (unsigned long)n * 8 };
    long pid = shim_raw_syscall6(172, 0, 0, 0, 0, 0, 0);
    return shim_raw_syscall6(270, pid, (long)&lv, 1, (long)&rv, 1, 0) == n * 8;
}
static void peek_line(const char *tag, unsigned long a, int n) {
    unsigned long q[8]; char b[300]; char *i = b; const char *p = tag;
    while (*p) *i++ = *p++;
    shim_hex(&i, a, 16); *i++ = ':';
    if (n > 8) n = 8;
    if (a && peek_q(a, q, n)) {
        for (int k = 0; k < n; k++) { *i++ = ' '; shim_hex(&i, q[k], 16); }
    } else { p = " <neciteln>"; while (*p) *i++ = *p++; }
    *i++ = '\n';
    long fd = shim_raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL,
                                 (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/trace_call.txt",
                                 0x441L, 0644L, 0, 0);
    if (fd >= 0) { shim_raw_syscall6(64, fd, (long)b, (long)(i - b), 0, 0, 0); shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0); }
}
static int g_trace_peek = -1;
static void trace_regs_peek_logger(unsigned long *regs) {
    trace_regs_logger(regs);
    unsigned long sfi = 0;
    if (peek_q(regs[0], &sfi, 1) && (sfi & 1)) peek_line("  *x0-1=", sfi - 1, 8);
    if (regs[2] & 1) peek_line("  x2-1=", regs[2] - 1, 8);
    static const int ids[] = { 83, 104, 198, 518 };
    for (int k = 0; k < 4; k++)
        peek_line("  btab=", regs[1] + 0xa798 + (unsigned long)ids[k] * 8, 1);
}
static void install_call_trace(void *target) {
    install_call_trace_with(target, (void *)trace_call_logger, "TRACE_CALL");
}
static void install_ring_trace(void *target) {
    install_call_trace_with(target, (void *)ring_logger, "TRACE_RING");
}
static void install_pause_call_trace(void *target) {
    install_call_trace_with(target, (void *)trace_call_pause_logger, "PAUSE_CALL");
}

/* ELF_LOADER_TRACE_ENTRY=<hex_addr>: jako install_call_trace, ale pro
 * instrukce, ktere NEJSOU `blr` (napr. entry do Builtins_InterpreterEntry-
 * Trampoline, `ldur x5,[x1,#31]`). Rozdil oproti install_call_trace:
 * puvodni instrukce NEMENI x30 (LR) - kdyz nas shim provede VLASTNI blr
 * (volani loggeru), MUSI x30 pred tim ulozit a po nem obnovit, jinak by
 * trampolina po navratu z InterpreterEntryTrampoline skocila do nasi
 * logovaci funkce misto ke skutecnemu volajicimu. Loguje x1 (JSFunction)
 * a puvodni x30 (adresa volajiciho) - odhali KDO volal do trampoliny
 * tesne pred padem. Instrukce se PREBIRA BEZE ZMENY (nekontroluje se typ,
 * volajici musi vedet, ze je bezpecna k relokaci - zadna PC-relativni
 * zavislost). */
static void trace_entry_logger(unsigned long *regs) {
    char b[200]; char *i = b;
    const char *p = "[TRACE_ENTRY] x1(JSFunction)="; while (*p) *i++ = *p++;
    shim_hex(&i, regs[0], 16);
    p = " x30(caller)="; while (*p) *i++ = *p++;
    shim_hex(&i, regs[1], 16);
    *i++ = '\n';
    long fd = shim_raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL,
                                 (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/trace_call.txt",
                                 0x441L, 0644L, 0, 0);
    if (fd >= 0) {
        shim_raw_syscall6(64, fd, (long)b, (long)(i - b), 0, 0, 0);
        shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
    }
}
/* ELF_LOADER_PAUSE_ENTRY=<hex_addr>: jako install_entry_trace, ale logger
 * navic zavola raw nanosleep(30s) skrz shim_raw_syscall6 (SYS_nanosleep=101
 * na arm64). Ucel: "Debugger listening on ws://..." se vypise driv, nez
 * node zavola prvni JS (bootstrap) - pad je ale bezprostredne poté a
 * vnejsi TCP connect race (curl/dev-tcp) okno spolehlive netrefi (overeno,
 * desitky tisic pokusu bez zasahu). Hookovanim VSTUPU do prvniho volani
 * (napr. Builtins_JSEntryTrampoline pred JS bootstrapem) a vlozenim pevne
 * pauzy se okno prodlouzi na desitky sekund - trepan-ni/websocket klient
 * se pripoji BEZ zavodeni o cas. */
static void trace_entry_pause_logger(unsigned long *regs) {
    trace_entry_logger(regs);
    struct { long tv_sec; long tv_nsec; } ts = { 30, 0 };
    shim_raw_syscall6(101 /* nanosleep */, (long)(unsigned long)&ts, 0, 0, 0, 0, 0);
}
static void install_entry_trace_with(void *target, void *logger_fn, const char *tag) {
    uint32_t ins0 = *(const uint32_t *)target;
    void *tramp = alloc_near(target);
    if (tramp == MAP_FAILED) { fprintf(stderr, "[%s] alloc_near(tramp) FAIL\n", tag); return; }
    *(uint32_t *)tramp = ins0;
    uint32_t back = branch_insn((char *)tramp + 4, (char *)target + 4);
    if (back) {
        *(uint32_t *)((char *)tramp + 4) = back;
    } else {
        void *bb = make_bridge((char *)target + 4);
        if (!bb) { fprintf(stderr, "[%s] make_bridge FAIL\n", tag); return; }
        uint32_t b2 = branch_insn((char *)tramp + 4, bb);
        if (!b2) { fprintf(stderr, "[%s] tramp->back OOR\n", tag); return; }
        *(uint32_t *)((char *)tramp + 4) = b2;
    }
    __builtin___clear_cache(tramp, (char *)tramp + 8);
    mprotect(tramp, 4096, PROT_READ | PROT_EXEC);

    void *shim = alloc_near((char *)target + 64);
    if (shim == MAP_FAILED) { fprintf(stderr, "[%s] alloc_near(shim) FAIL\n", tag); return; }
    uint32_t *sc = (uint32_t *)shim;
    int i = 0;
    sc[i++] = 0xD10083FFu;                    /* sub sp, sp, #32 */
    sc[i++] = tc_enc_str(1, 31, 0);           /* str x1, [sp, #0]  (ulozit JSFunction) */
    sc[i++] = tc_enc_str(30, 31, 8);          /* str x30,[sp, #8]  (ulozit LR) */
    sc[i++] = 0x910003E0u;                    /* mov x0, sp (arg logger = &[x1,x30]) */
    sc[i++] = 0x14000003u;                    /* b +12 (preskoc literal) */
    uint64_t *lit = (uint64_t *)&sc[i];
    *lit = (uint64_t)(uintptr_t)logger_fn;
    i += 2;
    sc[i++] = 0x58FFFFC9u;                    /* ldr x9, [pc, #-8] */
    sc[i++] = 0xD63F0120u;                    /* blr x9 */
    sc[i++] = tc_enc_ldr(30, 31, 8);          /* ldr x30,[sp, #8]  (obnov PUVODNI LR) */
    sc[i++] = tc_enc_ldr(1, 31, 0);           /* ldr x1, [sp, #0]  (obnov PUVODNI x1) */
    sc[i++] = 0x910083FFu;                    /* add sp, sp, #32 */
    uint32_t jb = branch_insn((char *)shim + (size_t)i * 4, tramp);
    if (!jb) { fprintf(stderr, "[%s] shim->tramp OOR\n", tag); return; }
    sc[i++] = jb;
    __builtin___clear_cache(shim, (char *)shim + (size_t)i * 4);
    mprotect(shim, 4096, PROT_READ | PROT_EXEC);

    if (patch_branch(target, shim) == 0)
        fprintf(stderr, "[%s] installed at %p (tramp=%p shim=%p)\n", tag, target, tramp, shim);
    else
        fprintf(stderr, "[%s] patch_branch FAILED at %p\n", tag, target);
}
static void install_pause_entry_trace(void *target) {
    install_entry_trace_with(target, (void *)trace_entry_pause_logger, "PAUSE_ENTRY");
}
static void install_entry_trace(void *target) {
    uint32_t ins0 = *(const uint32_t *)target;
    void *tramp = alloc_near(target);
    if (tramp == MAP_FAILED) { fprintf(stderr, "[TRACE_ENTRY] alloc_near(tramp) FAIL\n"); return; }
    *(uint32_t *)tramp = ins0;
    uint32_t back = branch_insn((char *)tramp + 4, (char *)target + 4);
    if (back) {
        *(uint32_t *)((char *)tramp + 4) = back;
    } else {
        void *bb = make_bridge((char *)target + 4);
        if (!bb) { fprintf(stderr, "[TRACE_ENTRY] make_bridge FAIL\n"); return; }
        uint32_t b2 = branch_insn((char *)tramp + 4, bb);
        if (!b2) { fprintf(stderr, "[TRACE_ENTRY] tramp->back OOR\n"); return; }
        *(uint32_t *)((char *)tramp + 4) = b2;
    }
    __builtin___clear_cache(tramp, (char *)tramp + 8);
    mprotect(tramp, 4096, PROT_READ | PROT_EXEC);

    void *shim = alloc_near((char *)target + 64);
    if (shim == MAP_FAILED) { fprintf(stderr, "[TRACE_ENTRY] alloc_near(shim) FAIL\n"); return; }
    uint32_t *sc = (uint32_t *)shim;
    int i = 0;
    /* ulozit x1 (JSFunction, arg pro logger) a x30 (LR, MUSI se obnovit -
     * puvodni instrukce ho nemeni). sub sp,#32 (16-align, 16 B potreba).
     * Logger cte regs[0]=[sp,#0] (x1 puvodni), regs[1]=[sp,#8] (x30
     * puvodni) - primo z ulozeneho mista, zadna extra kopie netreba. */
    sc[i++] = 0xD10083FFu;                    /* sub sp, sp, #32 */
    sc[i++] = tc_enc_str(1, 31, 0);           /* str x1, [sp, #0]  (ulozit JSFunction) */
    sc[i++] = tc_enc_str(30, 31, 8);          /* str x30,[sp, #8]  (ulozit LR) */
    sc[i++] = 0x910003E0u;                    /* mov x0, sp (arg logger = &[x1,x30]) */
    sc[i++] = 0x14000003u;                    /* b +12 (preskoc literal) */
    uint64_t *lit = (uint64_t *)&sc[i];
    *lit = (uint64_t)(uintptr_t)trace_entry_logger;
    i += 2;
    sc[i++] = 0x58FFFFC9u;                    /* ldr x9, [pc, #-8] */
    sc[i++] = 0xD63F0120u;                    /* blr x9 */
    sc[i++] = tc_enc_ldr(30, 31, 8);          /* ldr x30,[sp, #8]  (obnov PUVODNI LR) */
    sc[i++] = tc_enc_ldr(1, 31, 0);           /* ldr x1, [sp, #0]  (obnov PUVODNI x1) */
    sc[i++] = 0x910083FFu;                    /* add sp, sp, #32 */
    uint32_t jb = branch_insn((char *)shim + (size_t)i * 4, tramp);
    if (!jb) { fprintf(stderr, "[TRACE_ENTRY] shim->tramp OOR\n"); return; }
    sc[i++] = jb;
    __builtin___clear_cache(shim, (char *)shim + (size_t)i * 4);
    mprotect(shim, 4096, PROT_READ | PROT_EXEC);

    if (patch_branch(target, shim) == 0)
        fprintf(stderr, "[TRACE_ENTRY] installed at %p (tramp=%p shim=%p)\n",
                target, tramp, shim);
    else
        fprintf(stderr, "[TRACE_ENTRY] patch_branch FAILED at %p\n", target);
}

/* ELF_LOADER_TRACE_STRROOT=<hex_addr>: jednorazovy experiment. Hookuje
 * vstup do `v8::internal::FactoryBase<LocalFactory>::eval_string()`
 * (x0 = factory/isolate "this"). Tato a analogicke *_string() funkce
 * pocitaji adresu interniho stringu jako `*(x0+1960) + staticky_offset`
 * (offset zjisten z disassembly: eval_string=6112, getOffsetNanosecondsFor
 * =6288, rozdil 176 B = 22 slotu). Hypoteza: base pointer `*(x0+1960)`
 * je pod loaderem posunuty o konstantu, takze "eval_string" hledani
 * systematicky trefi getOffsetNanosecondsFor slot - to by vysvetlilo,
 * proc VZDY tahle funkce padne v kazdem eval-modu (-e/-p/--check), bez
 * ohledu na --no-harmony-temporal/verzi node. Logger vypise RAW BAJTY
 * na obou adresach (eval_string ocekavana pozice + getOffsetNanosecondsFor
 * ocekavana pozice), aby se hypoteza dala primo overit/vyvratit. */
static void strroot_logger(unsigned long *regs) {
    unsigned long x0 = regs[0];
    char b[400]; char *i = b;
    const char *p = "[STRROOT] factory_this="; while (*p) *i++ = *p++;
    shim_hex(&i, x0, 16);
    if (x0 > 0x10000) {
        unsigned long base = *(volatile unsigned long *)(x0 + 1960);
        p = " base=[x0+1960]="; while (*p) *i++ = *p++;
        shim_hex(&i, base, 16);
        if (base > 0x10000) {
            unsigned long a1 = base + 6112;  /* ocekavano: eval_string */
            unsigned long a2 = base + 6288;  /* ocekavano: getOffsetNanosecondsFor_string */
            p = "\n[STRROOT] @eval_string(base+6112)="; while (*p) *i++ = *p++;
            shim_hex(&i, a1, 16);
            p = " bytes="; while (*p) *i++ = *p++;
            for (int k = 0; k < 24; k++) {
                unsigned char c = *(volatile unsigned char *)(a1 + (unsigned long)k);
                *i++ = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            p = "\n[STRROOT] @getOffsetNS(base+6288)="; while (*p) *i++ = *p++;
            shim_hex(&i, a2, 16);
            p = " bytes="; while (*p) *i++ = *p++;
            for (int k = 0; k < 24; k++) {
                unsigned char c = *(volatile unsigned char *)(a2 + (unsigned long)k);
                *i++ = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
        }
    }
    *i++ = '\n';
    long fd = shim_raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL,
                                 (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/trace_call.txt",
                                 0x441L, 0644L, 0, 0);
    if (fd >= 0) {
        shim_raw_syscall6(64, fd, (long)b, (long)(i - b), 0, 0, 0);
        shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
    }
}
static void install_strroot_probe(void *target) {
    uint32_t ins0 = *(const uint32_t *)target;
    void *tramp = alloc_near(target);
    if (tramp == MAP_FAILED) { fprintf(stderr, "[STRROOT] alloc_near(tramp) FAIL\n"); return; }
    *(uint32_t *)tramp = ins0;
    uint32_t back = branch_insn((char *)tramp + 4, (char *)target + 4);
    if (back) {
        *(uint32_t *)((char *)tramp + 4) = back;
    } else {
        void *bb = make_bridge((char *)target + 4);
        if (!bb) { fprintf(stderr, "[STRROOT] make_bridge FAIL\n"); return; }
        uint32_t b2 = branch_insn((char *)tramp + 4, bb);
        if (!b2) { fprintf(stderr, "[STRROOT] tramp->back OOR\n"); return; }
        *(uint32_t *)((char *)tramp + 4) = b2;
    }
    __builtin___clear_cache(tramp, (char *)tramp + 8);
    mprotect(tramp, 4096, PROT_READ | PROT_EXEC);

    void *shim = alloc_near((char *)target + 64);
    if (shim == MAP_FAILED) { fprintf(stderr, "[STRROOT] alloc_near(shim) FAIL\n"); return; }
    uint32_t *sc = (uint32_t *)shim;
    int i = 0;
    /* ulozit x0 (arg logger) a x30 (LR, MUSI se obnovit). */
    sc[i++] = 0xD10083FFu;                    /* sub sp, sp, #32 */
    sc[i++] = tc_enc_str(0, 31, 0);           /* str x0, [sp, #0] */
    sc[i++] = tc_enc_str(30, 31, 8);          /* str x30,[sp, #8] */
    sc[i++] = 0x910003E0u;                    /* mov x0, sp (arg logger = &[x0,x30]) */
    sc[i++] = 0x14000003u;                    /* b +12 (preskoc literal) */
    uint64_t *lit = (uint64_t *)&sc[i];
    *lit = (uint64_t)(uintptr_t)strroot_logger;
    i += 2;
    sc[i++] = 0x58FFFFC9u;                    /* ldr x9, [pc, #-8] */
    sc[i++] = 0xD63F0120u;                    /* blr x9 */
    sc[i++] = tc_enc_ldr(30, 31, 8);          /* ldr x30,[sp, #8]  (obnov PUVODNI LR) */
    sc[i++] = tc_enc_ldr(0, 31, 0);           /* ldr x0, [sp, #0]  (obnov PUVODNI x0) */
    sc[i++] = 0x910083FFu;                    /* add sp, sp, #32 */
    uint32_t jb = branch_insn((char *)shim + (size_t)i * 4, tramp);
    if (!jb) { fprintf(stderr, "[STRROOT] shim->tramp OOR\n"); return; }
    sc[i++] = jb;
    __builtin___clear_cache(shim, (char *)shim + (size_t)i * 4);
    mprotect(shim, 4096, PROT_READ | PROT_EXEC);

    if (patch_branch(target, shim) == 0)
        fprintf(stderr, "[STRROOT] installed at %p (tramp=%p shim=%p)\n",
                target, tramp, shim);
    else
        fprintf(stderr, "[STRROOT] patch_branch FAILED at %p\n", target);
}

/* Nahradi prvni instrukci targetu vetvim na shim. Vytvori trampolinu orig,
 * ktera zavola realni glibc funkci (puvodni prolog + navrat, nebo nasledovani
 * B-thunku na realni impl). Vraci 0, pokud nelze (PC-relativni prolog). */
static int hook_install(f2_hook_t *h) {
    void *target = elf_scope_lookup(g_shim_scope, h->n);
    if (!target) { if (elf_debug()) fprintf(stderr, "[hook] %s: NOTFOUND\n", h->n); return 0; }
    if (elf_debug()) fprintf(stderr, "[hook] %s target=%p ins0=%08x\n", h->n, target, *(const uint32_t *)target);
    uint32_t ins0 = *(const uint32_t *)target;
    uint32_t cls = ins0 & 0xFC000000;
    if (cls == 0x94000000) { if (elf_debug()) fprintf(stderr, "[hook] %s: BL prolog, skip\n", h->n); return 0; }
    if ((ins0 & 0x9F000000) == 0x90000000) { if (elf_debug()) fprintf(stderr, "[hook] %s: ADRP prolog, skip\n", h->n); return 0; }
    if ((ins0 & 0xBF000000) == 0x18000000) { if (elf_debug()) fprintf(stderr, "[hook] %s: LDRlit prolog, skip\n", h->n); return 0; }
    /* BTI: funkce s BTI landing padem (napr. open ma ins0=d503233f) nelze
     * trampolinovat -> skocit na target+4 zpusobi BTI fault (guarded page).
     * Hookujeme jen B-thunky (stat->__xstat, openat64->__openat64 atd.),
     * jejichz trampolina je B na realni impl (BTI-valid). Ostatni nechame na
     * PLT-override + g_orig_* fallback (volaji realni BTI-entry primo). */
    if (cls != 0x14000000) { if (elf_debug()) fprintf(stderr, "[hook] %s: non-B-thunk (ins0=%08x), skip\n", h->n, ins0); return 0; }
    void *tramp = alloc_near(target);
    if (tramp == MAP_FAILED) { if (elf_debug()) fprintf(stderr, "[hook] %s: alloc_near(tramp) FAIL\n", h->n); return 0; }
    if (elf_debug()) fprintf(stderr, "[hook] %s tramp=%p dist=%ld\n", h->n, tramp, (long)((intptr_t)tramp - (intptr_t)target));
    if (cls == 0x14000000) {
        /* B-thunk (stat->__stat, openat64->__openat64 atd.): nasledujeme vetev */
        int32_t imm26 = (int32_t)(ins0 << 6) >> 6;
        void *real = (void *)((uintptr_t)target + (uintptr_t)(imm26 << 2));
        uint32_t bi = branch_insn(tramp, real);
        if (bi) {
            *(uint32_t *)tramp = bi;
            __builtin___clear_cache(tramp, (char *)tramp + 4);
        } else {
            void *b = make_bridge(real);
            if (!b) return 0;
            uint32_t b2 = branch_insn(tramp, b);
            if (!b2) return 0;
            *(uint32_t *)tramp = b2;
            __builtin___clear_cache(tramp, (char *)tramp + 4);
        }
        mprotect(tramp, 4096, PROT_READ | PROT_EXEC);
        *h->orig = tramp;
        if (patch_branch(target, h->shim) != 0) return 0;
        if (elf_debug()) fprintf(stderr, "[hook] %s: thunk->shim (real=%p)\n", h->n, real);
        return 1;
    }
    *(uint32_t *)tramp = ins0;
    uint32_t bi = branch_insn((char *)tramp + 4, (char *)target + 4);
    if (bi) {
        *(uint32_t *)((char *)tramp + 4) = bi;
        __builtin___clear_cache(tramp, (char *)tramp + 8);
    } else {
        void *b = make_bridge((char *)target + 4);
        if (!b) { if (elf_debug()) fprintf(stderr, "[hook] %s: make_bridge(tramp+4) NULL\n", h->n); return 0; }
        uint32_t b2 = branch_insn((char *)tramp + 4, b);
        if (!b2) { if (elf_debug()) fprintf(stderr, "[hook] %s: tramp+4->bridge OOR (tramp=%p b=%p)\n", h->n, tramp, b); return 0; }
        *(uint32_t *)((char *)tramp + 4) = b2;
        __builtin___clear_cache(tramp, (char *)tramp + 16);
    }
    mprotect(tramp, 4096, PROT_READ | PROT_EXEC);
    *h->orig = tramp;
    if (patch_branch(target, h->shim) != 0) return 0;
    if (elf_debug()) fprintf(stderr, "[hook] %s: patched\n", h->n);
    return 1;
}

typedef int (*fp_open)(const char *, int, ...);
static int g_loader_active = 0;        /* 1 = loaderuv vlastni kod (bionic TLS) */
typedef int (*fp_openat)(int, const char *, int, ...);
static int shim_resolve_symlinks(const char *path, char *out, size_t outsz);
static int shim_open(const char *p, int flags, ...) {
    char buf[8192]; const char *path = p;
    if (shim_translate(p, buf, sizeof buf)) path = buf;
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    fp_open f = (fp_open)g_orig_open;
    return f ? f(path, flags, mode) : -1;
}

/* POZNAMKA: shim funkce bezi v kontextu ciloveho vlakna, jehoz TLS je parrot
 * glibc (ne bionic). Proto NESMI volat bionic libc (fprintf/fopen/...), to by
 * dereferencovalo bionic errno/TLS -> NULL. Pouzivame jen ciste operace
 * (strlen/strncmp/snprintf jsou bez TLS) a vysledek volame pres shim_real,
 * coz je parrot glibc funkce (bezi ve spravnem glibc TLS kontextu). */
static int shim_open64(const char *p, int flags, ...) {
    char buf[8192]; const char *path = p;
    if (shim_translate(p, buf, sizeof buf)) path = buf;
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    fp_open f = (fp_open)g_orig_open64;
    return f ? f(path, flags, mode) : -1;
}
static int shim_openat(int dfd, const char *p, int flags, ...) {
    char buf[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') {
        if (shim_translate(p, buf, sizeof buf)) path = buf;
        char resolved[8192];
        if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    }
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    fp_openat f = (fp_openat)g_orig_openat;
    return f ? f(dfd, path, flags, mode) : -1;
}
static int shim_openat64(int dfd, const char *p, int flags, ...) {
    char buf[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') {
        if (shim_translate(p, buf, sizeof buf)) path = buf;
        char resolved[8192];
        if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    }
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    fp_openat f = (fp_openat)g_orig_openat64;
    return f ? f(dfd, path, flags, mode) : -1;
}

typedef int (*fp_stat)(const char *, struct stat *);
typedef int (*fp_stat64)(const char *, struct stat64 *);
typedef int (*fp_xstat)(int, const char *, struct stat *);
typedef int (*fp_lstat)(const char *, struct stat *);
typedef int (*fp_lxstat)(int, const char *, struct stat *);
typedef int (*fp_lstat64)(const char *, struct stat64 *);
typedef int (*fp_fstat64)(int, struct stat64 *);
typedef int (*fp_fstatat64)(int, const char *, struct stat64 *, int);
typedef int (*fp_statvfs)(const char *, struct statvfs *);
typedef int (*fp_statvfs64)(const char *, struct statvfs64 *);
typedef int (*fp_access)(const char *, int);
typedef int (*fp_euidaccess)(const char *, int);
typedef int (*fp_faccessat)(int, const char *, int, int);

/* Forward declaration */
static int shim_resolve_symlinks(const char *path, char *out, size_t outsz);

static int shim_stat(const char *p, struct stat *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    fp_stat f = (fp_stat)g_orig_stat; return f ? f(path, st) : -1;
}
static int shim_stat64(const char *p, struct stat64 *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    fp_stat64 f = (fp_stat64)g_orig_stat64; return f ? f(path, st) : -1;
}
static int shim___xstat(int v, const char *p, struct stat *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    fp_xstat f = (fp_xstat)g_orig___xstat; return f ? f(v, path, st) : -1;
}
static int shim_lstat(const char *p, struct stat *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_lstat f = (fp_lstat)g_orig_lstat; return f ? f(path, st) : -1;
}
static int shim___lxstat(int v, const char *p, struct stat *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_lxstat f = (fp_lxstat)g_orig___lxstat; return f ? f(v, path, st) : -1;
}
/* glibc >= 2.33 na aarch64 exportuje realne LFS symboly lstat64/stat64/fstat64.
 * lstat64 chybel v g_f2_hooks -> git importoval neprelozeny lstat64 proti
 * host rootu (cannot stat template '/usr/share/git-core/...'). lstat64
 * ZAMERNE neresi symlinky (lstat nesmi nasledovat posledni symlink). */
static int shim_lstat64(const char *p, struct stat64 *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_lstat64 f = (fp_lstat64)g_orig_lstat64; return f ? f(path, st) : -1;
}
static int shim_fstat64(int fd, struct stat64 *st) {
    fp_fstat64 f = (fp_fstat64)g_orig_fstat64; return f ? f(fd, st) : -1;
}
static int shim_fstatat64(int dfd, const char *p, struct stat64 *st, int flags) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_fstatat64 f = (fp_fstatat64)g_orig_fstatat64; return f ? f(dfd, path, st, flags) : -1;
}
static int shim_statvfs(const char *p, struct statvfs *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    fp_statvfs f = (fp_statvfs)g_orig_statvfs; return f ? f(path, st) : -1;
}
static int shim_statvfs64(const char *p, struct statvfs64 *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    fp_statvfs64 f = (fp_statvfs64)g_orig_statvfs64; return f ? f(path, st) : -1;
}

static int shim_access(const char *p, int m) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    /* Resolve symlinks under ROOTFS so kernel doesn't follow guest-absolute
     * symlink targets against the host root (e.g. awk -> /etc/alternatives/awk) */
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) {
        path = resolved;
    }
    fp_access f = (fp_access)g_orig_access; return f ? f(path, m) : -1;
}
static int shim_euidaccess(const char *p, int m) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    /* Resolve symlinks under ROOTFS so kernel doesn't follow guest-absolute
     * symlink targets against the host root (e.g. awk -> /etc/alternatives/awk) */
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) {
        path = resolved;
    }
    fp_euidaccess f = (fp_euidaccess)g_orig_euidaccess; return f ? f(path, m) : -1;
}
static int shim_faccessat(int dfd, const char *p, int m, int ff) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') {
        if (shim_translate(p, b, sizeof b)) path = b;
        char resolved[8192];
        if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) {
            path = resolved;
        }
    }
    fp_faccessat f = (fp_faccessat)g_orig_faccessat; return f ? f(dfd, path, m, ff) : -1;
}

/* Moderni glibc/coreutils routuji stat/lstat/fstatat pres statx syscall.
 * Bez tohoto hooku zustane statx netranslatovany -> ENOENT na hostu. */
typedef int (*fp_statx)(int, const char *, int, unsigned int, void *);
static int shim_statx(int dfd, const char *p, int flags, unsigned int mask, void *stx) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') {
        if (shim_translate(p, b, sizeof b)) path = b;
        if (!(flags & 0x100 /* AT_SYMLINK_NOFOLLOW */)) {
            char resolved[8192];
            if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
        }
    }
    fp_statx f = (fp_statx)g_orig_statx; return f ? f(dfd, path, flags, mask, stx) : -1;
}
typedef int (*fp_fstatat)(int, const char *, void *, int);
static int shim_fstatat(int dfd, const char *p, void *st, int flags) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') {
        if (shim_translate(p, b, sizeof b)) path = b;
        if (!(flags & 0x100 /* AT_SYMLINK_NOFOLLOW */)) {
            char resolved[8192];
            if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
        }
    }
    fp_fstatat f = (fp_fstatat)g_orig_fstatat; return f ? f(dfd, path, st, flags) : -1;
}
static int shim_newfstatat(int dfd, const char *p, void *st, int flags) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') {
        if (shim_translate(p, b, sizeof b)) path = b;
        if (!(flags & 0x100 /* AT_SYMLINK_NOFOLLOW */)) {
            char resolved[8192];
            if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
        }
    }
    fp_fstatat f = (fp_fstatat)g_orig_newfstatat; return f ? f(dfd, path, st, flags) : -1;
}

/* Path-mutujici operace: dpkg/ldconfig/apt vytvareji symlinky, mkdir, rename.
 * Bez prekladu by zapisovaly na host cesty (ldapfig Can't link, dpkg chyby). */
typedef int (*fp_symlink)(const char *, const char *);
static int shim_symlink(const char *t, const char *lp) {
    char b[8192]; const char *linkpath = lp;
    if (shim_translate(lp, b, sizeof b)) linkpath = b;
    fp_symlink f = (fp_symlink)g_orig_symlink; return f ? f(t, linkpath) : -1;
}
typedef int (*fp_symlinkat)(const char *, int, const char *);
static int shim_symlinkat(const char *t, int ndfd, const char *lp) {
    char b[8192]; const char *linkpath = lp;
    if (ndfd == -100 && lp && lp[0] == '/') { if (shim_translate(lp, b, sizeof b)) linkpath = b; }
    fp_symlinkat f = (fp_symlinkat)g_orig_symlinkat; return f ? f(t, ndfd, linkpath) : -1;
}
typedef int (*fp_link)(const char *, const char *);
static int shim_link(const char *o, const char *n) {
    char b1[8192], b2[8192]; const char *oldp = o, *newp = n;
    if (shim_translate(o, b1, sizeof b1)) oldp = b1;
    if (shim_translate(n, b2, sizeof b2)) newp = b2;
    fp_link f = (fp_link)g_orig_link; return f ? f(oldp, newp) : -1;
}
typedef int (*fp_rename)(const char *, const char *);
static int shim_rename(const char *o, const char *n) {
    char b1[8192], b2[8192]; const char *oldp = o, *newp = n;
    if (shim_translate(o, b1, sizeof b1)) oldp = b1;
    if (shim_translate(n, b2, sizeof b2)) newp = b2;
    fp_rename f = (fp_rename)g_orig_rename; return f ? f(oldp, newp) : -1;
}
/* GNU coreutils mv (a dalsi) volaji renameat2/renameat, NE rename. Bez techto
 * shimu zustala cesta netranslatovana -> mv hledal /tmp/x na HOSTU misto
 * $ROOTFS/tmp/x -> ENOENT ("cannot move ... No such file or directory"). */
typedef int (*fp_renameat)(int, const char *, int, const char *);
static int shim_renameat(int odfd, const char *o, int ndfd, const char *n) {
    char b1[8192], b2[8192]; const char *oldp = o, *newp = n;
    if (odfd == -100 && o && o[0] == '/') { if (shim_translate(o, b1, sizeof b1)) oldp = b1; }
    if (ndfd == -100 && n && n[0] == '/') { if (shim_translate(n, b2, sizeof b2)) newp = b2; }
    fp_renameat f = (fp_renameat)g_orig_renameat; return f ? f(odfd, oldp, ndfd, newp) : -1;
}
typedef int (*fp_renameat2)(int, const char *, int, const char *, unsigned int);
static int shim_renameat2(int odfd, const char *o, int ndfd, const char *n, unsigned int flags) {
    char b1[8192], b2[8192]; const char *oldp = o, *newp = n;
    if (odfd == -100 && o && o[0] == '/') { if (shim_translate(o, b1, sizeof b1)) oldp = b1; }
    if (ndfd == -100 && n && n[0] == '/') { if (shim_translate(n, b2, sizeof b2)) newp = b2; }
    fp_renameat2 f = (fp_renameat2)g_orig_renameat2; return f ? f(odfd, oldp, ndfd, newp, flags) : -1;
}
typedef int (*fp_unlink)(const char *);
static int shim_unlink(const char *p) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_unlink f = (fp_unlink)g_orig_unlink; return f ? f(path) : -1;
}
/* GNU coreutils rm/rmdir volaji unlinkat/rmdirat (NE unlink/rmdir). Bez techto
 * shimu zustala cesta netranslatovana -> rm hledal /tmp/x na HOSTU misto
 * $ROOTFS/tmp/x -> ENOENT ("cannot remove ... No such file or directory"). */
typedef int (*fp_unlinkat)(int, const char *, int);
static int shim_unlinkat(int dfd, const char *p, int flags) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_unlinkat f = (fp_unlinkat)g_orig_unlinkat; return f ? f(dfd, path, flags) : -1;
}
typedef int (*fp_rmdirat)(int, const char *);
static int shim_rmdirat(int dfd, const char *p) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_rmdirat f = (fp_rmdirat)g_orig_rmdirat; return f ? f(dfd, path) : -1;
}
typedef int (*fp_mkdir)(const char *, unsigned int);
static int shim_mkdir(const char *p, unsigned int m) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_mkdir f = (fp_mkdir)g_orig_mkdir; return f ? f(path, m) : -1;
}
typedef int (*fp_mkdirat)(int, const char *, unsigned int);
static int shim_mkdirat(int dfd, const char *p, unsigned int m) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_mkdirat f = (fp_mkdirat)g_orig_mkdirat; return f ? f(dfd, path, m) : -1;
}
typedef int (*fp_rmdir)(const char *);
static int shim_rmdir(const char *p) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_rmdir f = (fp_rmdir)g_orig_rmdir; return f ? f(path) : -1;
}

typedef int (*fp_execve)(const char *, char *const[], char *const[]);
typedef int (*fp_execv)(const char *, char *const[]);
typedef int (*fp_execvp)(const char *, char *const[]);
typedef int (*fp_execvpe)(const char *, char *const[], char *const[]);
typedef int (*fp_execveat)(int, const char *, char *const[], char *const[], int);

#ifndef SYS_faccessat
#define SYS_faccessat 48
#endif
#ifndef SYS_openat
#define SYS_openat 56
#endif
#ifndef SYS_read
#define SYS_read 63
#endif
#ifndef SYS_close
#define SYS_close 57
#endif
#ifndef SYS_prlimit64
#define SYS_prlimit64 261
#endif

static int raw_access(const char *path, int mode) {
    if (!path) return -1;
    return (int)shim_raw_syscall6(SYS_faccessat, -100 /* AT_FDCWD */, (long)path, mode, 0, 0, 0);
}

static int raw_open(const char *path, int flags) {
    if (!path) return -1;
    return (int)shim_raw_syscall6(SYS_openat, -100 /* AT_FDCWD */, (long)path, flags, 0, 0, 0);
}

static ssize_t raw_read(int fd, void *buf, size_t count) {
    return (ssize_t)shim_raw_syscall6(SYS_read, fd, (long)buf, (long)count, 0, 0, 0);
}

static int raw_close(int fd) {
    return (int)shim_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
}

static char *raw_getcwd(char *buf, size_t sz) {
    long r = shim_raw_syscall6(17 /* SYS_getcwd */, (long)buf, (long)sz, 0, 0, 0, 0);
    return r > 0 ? buf : NULL;
}

static ssize_t raw_readlinkat(const char *path, char *buf, size_t sz) {
    return (ssize_t)shim_raw_syscall6(78 /* SYS_readlinkat */, -100, (long)path, (long)buf, (long)sz, 0, 0);
}

/* Check if a path exists without following symlinks. Needed for PATH search
 * because guest symlinks (e.g. awk -> /etc/alternatives/awk) point to
 * guest-absolute targets that don't exist on the host. raw_access(X_OK)
 * follows symlinks against the host root and fails. newfstatat with
 * AT_SYMLINK_NOFOLLOW checks the symlink itself. */
static int raw_path_exists(const char *path) {
    /* struct stat is ~128 bytes on aarch64; use 256 to be safe */
    unsigned char st[256] __attribute__((aligned(16)));
    /* newfstatat=79, AT_FDCWD=-100, AT_SYMLINK_NOFOLLOW=0x100 */
    long r = shim_raw_syscall6(79, -100, (long)path, (long)st, 0x100, 0, 0);
    return r == 0;
}

/* 1 = cilova binarka je glibc ELF (PT_INTERP obsahuje "ld-linux"), tedy se
 * NESMI spustit primym bionickym execve (host nema /lib/ld-linux-aarch64.so.1),
 * ale MUSI projit pres loader - i kdyz lezi MIMO ROOTFS (napr. uv managed
 * Python pod /data/user/0/com.linux_core/files/.local/share/uv/python/...).
 * Bionicke binarky (linker64) vraceji 0. Raw syscally: bezpecne pod parrot TP. */
static int is_glibc_elf(const char *path) {
    int fd = raw_open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char h[64];
    ssize_t n = raw_read(fd, h, sizeof h);
    if (n < 64 || h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') {
        raw_close(fd);
        return 0;
    }
    uint64_t phoff = *(uint64_t *)(void *)(h + 32);
    uint16_t phentsize = *(uint16_t *)(void *)(h + 54);
    uint16_t phnum = *(uint16_t *)(void *)(h + 56);
    if (phentsize < 56 || phnum == 0 || phnum > 1024) { raw_close(fd); return 0; }
    int found = 0;
    for (int i = 0; i < phnum && !found; i++) {
        unsigned char ph[64];
        shim_raw_syscall6(62 /*lseek*/, fd,
                          (long)(phoff + (uint64_t)i * phentsize), 0, 0, 0, 0);
        if (raw_read(fd, ph, sizeof ph) < 56) break;
        uint32_t p_type = *(uint32_t *)(void *)ph;
        if (p_type != 3 /*PT_INTERP*/) continue;
        uint64_t p_offset = *(uint64_t *)(void *)(ph + 8);
        uint64_t p_filesz = *(uint64_t *)(void *)(ph + 32);
        if (p_filesz == 0 || p_filesz > 256) continue;
        char interp[300];
        shim_raw_syscall6(62, fd, (long)p_offset, 0, 0, 0, 0);
        ssize_t r = raw_read(fd, interp, p_filesz);
        if (r > 0) {
            interp[r] = 0;
            for (ssize_t k = 0; k + 8 <= r; k++)
                if (interp[k] == 'l' && shim_strncmp(interp + k, "ld-linux", 8) == 0) {
                    found = 1; break;
                }
        }
    }
    raw_close(fd);
    return found;
}

/* TLS-safe symlink resolver for paths under ROOTFS. Resolves symlink chains
 * where targets are guest-absolute (e.g. /etc/alternatives/awk -> /usr/bin/mawk)
 * by prepending ROOTFS to absolute targets. Returns 1 if resolved, 0 if not a
 * symlink or resolution failed. */
static int shim_resolve_symlinks(const char *path, char *out, size_t outsz) {
    size_t rl = g_shim_root ? shim_strlen(g_shim_root) : 0;
    if (!rl || !path || !path[0]) return 0;

    char cur[8192];
    shim_strcpy(cur, sizeof(cur), path);

    for (int depth = 0; depth < 32; depth++) {
        /* Check if cur is a symlink using newfstatat with AT_SYMLINK_NOFOLLOW */
        unsigned char st[256] __attribute__((aligned(16)));
        long r = shim_raw_syscall6(79, -100, (long)cur, (long)st, 0x100, 0, 0);
        if (r != 0) return 0; /* stat failed, not a symlink or doesn't exist */

        /* st_mode is at offset 16 in struct stat on aarch64, 4 bytes */
        unsigned int mode = *(unsigned int *)(st + 16);
        if ((mode & 0170000) != 0120000) {
            /* Not a symlink, we're done */
            shim_strcpy(out, outsz, cur);
            return 1;
        }

        /* Read the symlink target */
        char linkbuf[8192];
        ssize_t lr = raw_readlinkat(cur, linkbuf, sizeof(linkbuf) - 1);
        if (lr <= 0) return 0;
        linkbuf[lr] = 0;

        /* Resolve the target */
        char next[8192];
        size_t rrl = shim_strlen(g_shim_root);
        if (linkbuf[0] == '/' && shim_strncmp(linkbuf, g_shim_root, rrl) == 0 &&
            (linkbuf[rrl] == '/' || linkbuf[rrl] == 0)) {
            /* Uz pod ROOTFS (proot link2symlink .l2s cile jsou hostove absolutni) */
            shim_strcpy(next, sizeof(next), linkbuf);
        } else if (linkbuf[0] == '/') {
            /* Absolute target: prepend ROOTFS */
            shim_strcpy(next, sizeof(next), g_shim_root);
            shim_strcat(next, sizeof(next), linkbuf);
        } else {
            /* Relative target: resolve against symlink's directory */
            shim_strcpy(next, sizeof(next), cur);
            /* Find last '/' and truncate */
            char *last_slash = NULL;
            for (char *s = next; *s; s++) { if (*s == '/') last_slash = s; }
            if (last_slash) {
                *(last_slash + 1) = 0;
                shim_strcat(next, sizeof(next), linkbuf);
            } else {
                shim_strcpy(next, sizeof(next), linkbuf);
            }
        }
        shim_strcpy(cur, sizeof(cur), next);
    }
    /* Max depth reached */
    shim_strcpy(out, outsz, cur);
    return 1;
}

static const char *get_env_val(const char *key, char *const envp[]) {
    size_t klen = shim_strlen(key);
    char *const *e = envp ? envp : guest_environ();
    if (e) {
        for (int i = 0; e[i]; i++) {
            if (shim_strncmp(e[i], key, klen) == 0 && e[i][klen] == '=')
                return e[i] + klen + 1;
        }
    }
    return NULL;
}

static int search_guest_path(const char *p, char *const envp[], char *out, size_t out_cap) {
    size_t pl = shim_strlen(p);
    size_t rl = g_shim_root ? shim_strlen(g_shim_root) : 0;
    const char *pth = get_env_val("PATH", envp);
    if (pth && pth[0]) {
        const char *d = pth;
        while (*d) {
            const char *colon = d;
            while (*colon && *colon != ':') colon++;
            size_t dl = (size_t)(colon - d);
            if (dl > 0 && dl + pl + rl + 2 < 8192) {
                char cand[8192];
                size_t off = 0;
                if (rl && dl >= rl && shim_strncmp(d, g_shim_root, rl) == 0) {
                    shim_memcpy(cand, d, dl); off = dl;
                } else if (rl && d[0] == '/') {
                    shim_memcpy(cand, g_shim_root, rl); off = rl;
                    shim_memcpy(cand + off, d, dl); off += dl;
                } else {
                    shim_memcpy(cand, d, dl); off = dl;
                }
                cand[off++] = '/';
                shim_memcpy(cand + off, p, pl); off += pl;
                cand[off] = 0;
                if (raw_access(cand, X_OK) == 0 || raw_path_exists(cand)) {
                    shim_strcpy(out, out_cap, cand);
                    return 1;
                }
            }
            if (*colon == ':') colon++;
            d = colon;
        }
    }

    static const char *dirs[] = {
        "/usr/local/bin", "/usr/bin", "/bin",
        "/usr/local/sbin", "/usr/sbin", "/sbin", NULL
    };
    if (rl) {
        for (int i = 0; dirs[i]; i++) {
            char candidate[8192];
            size_t dl = shim_strlen(dirs[i]);
            size_t off = 0;
            shim_memcpy(candidate, g_shim_root, rl); off = rl;
            shim_memcpy(candidate + off, dirs[i], dl); off += dl;
            candidate[off++] = '/';
            shim_memcpy(candidate + off, p, pl); off += pl;
            candidate[off] = 0;
            if (raw_access(candidate, X_OK) == 0 || raw_path_exists(candidate)) {
                shim_strcpy(out, out_cap, candidate);
                return 1;
            }
        }
    }
    return 0;
}

/* ===== whitelist + white.log (zpetna vazba pro re-exec) =====
 * Whitelist a logovaci cesta se inicializuji v shim_register_overrides()
 * (bionicky kontext, bezpecne getenv/strlen). Vlastni zapis do logu jde pres
 * raw syscally s WL_SENTINEL v x5, aby ho F2 path-filtr nechal projit (a
 * neprekladal uz hotovou device cestu). */
#define WL_SENTINEL 0x1234567890ABCDEFULL
#define WL_MAX 256
#define WL_NAME_LEN 128
static char g_wl_names[WL_MAX][WL_NAME_LEN];
static int  g_wl_count = -1;      /* -1 = nenacteno, 0 = prazdny/nenalezeno */
static char g_wl_logpath[8192];   /* $ROOTFS/root/elf_loader/white.log */
static int  g_wl_ready = 0;

static void wl_fd_puts(int fd, const char *s) {
    if (!s) s = "(null)";
    shim_raw_syscall6(64 /*write*/, fd, (long)s, (long)shim_strlen(s), 0, 0, 0);
}
static void wl_fd_putl(int fd, long v) {
    char b[24];
    int i = 0;
    if (v < 0) { wl_fd_puts(fd, "-"); v = -v; }
    if (!v) b[i++] = '0';
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) { char c = b[--i]; shim_raw_syscall6(64, fd, (long)&c, 1, 0, 0, 0); }
}
static const char *wl_base(const char *p) {
    const char *b = p ? p : "";
    for (const char *q = b; *q; q++)
        if (*q == '/') b = q + 1;
    return b;
}

/* Nacti whitelist + priprav log cestu. Volano z bionickeho kontextu. */
static void wl_init(void) {
    if (g_wl_ready) return;
    g_wl_ready = 1;
    if (!g_shim_root || !g_shim_root[0]) { g_wl_count = 0; return; }

    char dir[8192];
    shim_strcpy(dir, sizeof dir, g_shim_root);
    shim_strcat(dir, sizeof dir, "/root/elf_loader");
    shim_raw_syscall6(34 /*mkdirat*/, -100, (long)dir, 0755, 0, 0, 0);
    shim_strcpy(g_wl_logpath, sizeof g_wl_logpath, dir);
    shim_strcat(g_wl_logpath, sizeof g_wl_logpath, "/white.log");

    const char *env = getenv("ELF_LOADER_WHITELIST");
    char path[8192];
    if (env && env[0]) {
        shim_strcpy(path, sizeof path, env);
    } else {
        shim_strcpy(path, sizeof path, dir);
        shim_strcat(path, sizeof path, "/whitelist.txt");
    }

    int fd = (int)shim_raw_syscall6(56 /*openat*/, -100, (long)path, 0 /*O_RDONLY*/,
                                    0, 0, (long)WL_SENTINEL);
    if (fd < 0) { g_wl_count = 0; return; }
    char buf[16384];
    long n = shim_raw_syscall6(63 /*read*/, fd, (long)buf, (long)sizeof(buf) - 1,
                               0, 0, 0);
    shim_raw_syscall6(57 /*close*/, fd, 0, 0, 0, 0, 0);
    if (n <= 0) { g_wl_count = 0; return; }
    buf[n] = 0;
    g_wl_count = 0;
    char *s = buf;
    while (*s && g_wl_count < WL_MAX) {
        char *e = s;
        while (*e && *e != '\n' && *e != '\r') e++;
        char save = *e;
        *e = 0;
        char *a = s;
        while (*a == ' ' || *a == '\t') a++;
        if (*a && *a != '#') {
            char *z = a + shim_strlen(a);
            while (z > a && (z[-1] == ' ' || z[-1] == '\t')) *--z = 0;
            if (*a) shim_strcpy(g_wl_names[g_wl_count++], WL_NAME_LEN, a);
        }
        if (!save) break;
        s = e + 1;
    }
}

/* 1 = smi se redirectovat, 0 = ne. Bez whitelistu (0 polozek) -> vse. */
static int wl_match(const char *p) {
    if (g_wl_count <= 0) return 1;
    const char *b = wl_base(p);
    for (int i = 0; i < g_wl_count; i++)
        if (shim_strcmp(g_wl_names[i], b) == 0) return 1;
    return 0;
}

static int wl_log_open(void) {
    if (!g_wl_logpath[0]) return -1;
    long fd = shim_raw_syscall6(56, -100, (long)g_wl_logpath,
                                0x441 /*O_WRONLY|O_CREAT|O_APPEND*/, 0644, 0,
                                (long)WL_SENTINEL);
    return fd >= 0 ? (int)fd : -1;
}
static void wl_log_exec(const char *p, const char *resolved, int is_script,
                        const char *interp) {
    int fd = wl_log_open();
    if (fd < 0) return;
    wl_fd_puts(fd, "[exec] path="); wl_fd_puts(fd, p);
    wl_fd_puts(fd, " resolved="); wl_fd_puts(fd, resolved);
    if (is_script) { wl_fd_puts(fd, " script=1 interp="); wl_fd_puts(fd, interp); }
    wl_fd_puts(fd, "\n");
    shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
}
static void wl_log_skip(const char *p, const char *why) {
    int fd = wl_log_open();
    if (fd < 0) return;
    wl_fd_puts(fd, "[skip] path="); wl_fd_puts(fd, p);
    wl_fd_puts(fd, " reason="); wl_fd_puts(fd, why);
    wl_fd_puts(fd, "\n");
    shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
}
static void wl_log_fail(const char *p, const char *resolved, long rc) {
    int fd = wl_log_open();
    if (fd < 0) return;
    wl_fd_puts(fd, "[fail] path="); wl_fd_puts(fd, p);
    wl_fd_puts(fd, " resolved="); wl_fd_puts(fd, resolved);
    wl_fd_puts(fd, " rc="); wl_fd_putl(fd, rc);
    wl_fd_puts(fd, "\n");
    shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
}

/* ───────── child env propagation (rootfs for fork+exec children) ─────────
 * When the caller launches the loader without exporting ROOTFS (e.g. ashell -c
 * with just an absolute guest path), g_shim_root is NULL and the re-exec'd
 * child loader cannot locate the distro libdirs -> "dep libc.so not found".
 * We derive the distro root from the child binary path and guarantee ROOTFS,
 * ELF_ROOTFS and LD_LIBRARY_PATH are present in the envp handed to the child.
 * Only guest binaries go through this path (host/excluded execs return early
 * with the original envp). Static storage only: this runs under parrot TP. */
#define SHIM_CE_MAX 4096
static char *g_ce_ptrs[SHIM_CE_MAX];
static char  g_ce_str[32768];
static char  g_ce_root[2048];

/* Walk up from the file's directory looking for usr/lib/aarch64-linux-gnu. */
static const char *shim_derive_root_from_path(const char *file) {
    if (!file || !file[0]) return NULL;
    char dir[2048];
    shim_strcpy(dir, sizeof(dir), file);
    char *slash = dir;
    for (char *q = dir; *q; q++) if (*q == '/') slash = q;
    if (slash > dir) *slash = 0; else if (slash == dir) dir[1] = 0;
    for (int depth = 0; depth < 64; depth++) {
        char probe[2304];
        shim_strcpy(probe, sizeof(probe), dir);
        shim_strcat(probe, sizeof(probe), "/usr/lib/aarch64-linux-gnu");
        if (raw_path_exists(probe)) {
            shim_strcpy(g_ce_root, sizeof(g_ce_root), dir);
            return g_ce_root;
        }
        char *s2 = NULL;
        for (char *q = dir; *q; q++) if (*q == '/') s2 = q;
        if (!s2) break;
        if (s2 == dir) { dir[1] = 0; }
        else *s2 = 0;
    }
    return NULL;
}

static char **shim_child_envp(char *const envp[], const char *child_file) {
    const char *root = (g_shim_root && g_shim_root[0])
                           ? g_shim_root : shim_derive_root_from_path(child_file);
    if (!root || !root[0]) return (char **)envp;

    /* NEPRIDAVAT LD_LIBRARY_PATH: ditetem je znovu spusteny elf_loader
     * (bionicky dynamicky), jehoz vnejsi bionicky linker by pres
     * LD_LIBRARY_PATH natahl parrot libc.so (GNU ld skript, text) ->
     * "bad ELF magic". Vnitrni glibc loader si distro libdirs odvodi sam
     * z cesty exe (derive_distro_libdirs) a z ELF_ROOTFS. Staci tedy
     * propagovat ROOTFS + ELF_ROOTFS. */

    /* Build new envp: drop ROOTFS/ELF_ROOTFS, keep the rest. */
    int o = 0;
    for (int i = 0; envp && envp[i] && o < SHIM_CE_MAX - 8; i++) {
        if (shim_strncmp(envp[i], "ROOTFS=", 7) == 0) continue;
        if (shim_strncmp(envp[i], "ELF_ROOTFS=", 11) == 0) continue;
        g_ce_ptrs[o++] = envp[i];
    }
    char *sp = g_ce_str;
    char *send = g_ce_str + sizeof(g_ce_str);
    #define CE_PUSH(name, val) do { \
        const char *_n = (name), *_v = (val); \
        char *_d = sp; \
        while (*_n && _d < send - 2) *_d++ = *_n++; \
        if (_d < send - 1) *_d++ = '='; \
        while (*_v && _d < send - 2) *_d++ = *_v++; \
        if (_d < send - 1) *_d++ = 0; \
        if (o < SHIM_CE_MAX - 4) g_ce_ptrs[o++] = sp; \
        sp = _d; \
    } while (0)
    CE_PUSH("ROOTFS", root);
    CE_PUSH("ELF_ROOTFS", root);
    #undef CE_PUSH
    g_ce_ptrs[o] = NULL;
    return g_ce_ptrs;
}

static int shim_execve(const char *p, char *const argv[], char *const envp[]) {
    if (!p || !p[0]) return -1;

    char resolved[8192];
    resolved[0] = 0;
    int force_redirect = 0;   /* glibc ELF mimo ROOTFS: preskoc whitelist gate */
    size_t rl = g_shim_root ? shim_strlen(g_shim_root) : 0;

    /* Symlink pre-resolve: device-side symlinky (napr. $D/usr/bin/git ->
     * $R/usr/bin/git) padaji jinak do shim_excluded() (cesty pod /data), coz je poslalo
     * na raw execve a bionic je nespusti (chybi PT_INTERP). Kdyz cil symlinku
     * lezi pod ROOTFS, pouzij ho jako 'p'.
     *
     * Android alias /data/user/0/ == /data/data/: symlink target casto pouziva
     * /data/user/0/... zatimco ROOTFS je /data/data/... - normalizujeme oba
     * prefixy pred porovnanim. */
    if (rl && p[0] == '/' &&
        !(shim_strncmp(p, g_shim_root, rl) == 0 && (p[rl] == '/' || p[rl] == 0))) {
        char linkbuf[8192];
        ssize_t lr = raw_readlinkat(p, linkbuf, sizeof(linkbuf) - 1);
        if (lr > 0) {
            linkbuf[lr] = 0;
            char norm[8192];
            const char *cand = linkbuf;
            if (shim_strncmp(linkbuf, "/data/user/0/", 13) == 0) {
                shim_strcpy(norm, sizeof(norm), "/data/data/");
                shim_strcat(norm, sizeof(norm), linkbuf + 13);
                cand = norm;
            }
            if (linkbuf[0] == '/' &&
                shim_strncmp(cand, g_shim_root, rl) == 0 &&
                (cand[rl] == '/' || cand[rl] == 0)) {
                shim_strcpy(resolved, sizeof(resolved), cand);
                p = resolved;
            }
        }
    }

    /* Path resolution — check ROOTFS prefix FIRST, before exclusion list.
     * ROOTFS lives under /data/... which is in the exclude list; without this
     * ordering, guest binaries referenced by device-absolute path would be
     * passed to the raw execve and die on missing PT_INTERP. */
    if (rl && shim_strncmp(p, g_shim_root, rl) == 0 && (p[rl] == '/' || p[rl] == 0)) {
        /* Already prefixed with $ROOTFS. Vetsina obsahu ROOTFS je glibc a
         * potrebuje own-loading, ale rootfs muze obsahovat i bionic-staticky
         * nastroj (napr. testovaci "ashell" v usr/local/bin). Takovy ELF se
         * MA spustit primo real execve - jinak ho loader zkousi own-loadovat
         * jako glibc a spadne hned na vstupu (FAULT-ENTER, tp=0, chybi
         * PT_INTERP/TLS layout). Skripty (#!) nejsou ELF, projdou beze zmeny
         * na shebang-handling nize. */
        unsigned char emag[4] = {0, 0, 0, 0};
        int efd = raw_open(p, O_RDONLY);
        int eopened = (efd >= 0);
        if (efd >= 0) { raw_read(efd, emag, 4); raw_close(efd); }
        int eglibc = is_glibc_elf(p);
        {
            /* TP-nezavisly raw diag (docasne, pro RCA) - stejny vzor jako
             * trace_call_logger: fprintf/getenv v teto vetvi cestou shim_execve
             * umi bezet pod spatnym TP a spadnout, proto jen raw syscally. */
            char b[400]; char *i = b;
            const char *s0 = "[rfdbg] p="; while (*s0) *i++ = *s0++;
            const char *q = p; while (*q && i < b + 300) *i++ = *q++;
            s0 = " opened="; while (*s0) *i++ = *s0++;
            *i++ = (char)('0' + (eopened ? 1 : 0));
            s0 = " magic="; while (*s0) *i++ = *s0++;
            shim_hex(&i, (unsigned long)((emag[0]<<24)|(emag[1]<<16)|(emag[2]<<8)|emag[3]), 8);
            s0 = " glibc="; while (*s0) *i++ = *s0++;
            *i++ = (char)('0' + (eglibc ? 1 : 0));
            *i++ = '\n';
            long fd = shim_raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL,
                (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/rfdbg.txt",
                0x441L, 0644L, 0, 0);
            if (fd >= 0) {
                shim_raw_syscall6(64, fd, (long)b, (long)(i - b), 0, 0, 0);
                shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
            }
        }
        if (emag[0] == 0x7f && emag[1] == 'E' && emag[2] == 'L' && emag[3] == 'F' &&
            !eglibc) {
            fp_execve f = (fp_execve)g_orig_execve;
            return f ? f(p, argv, envp) : -1;
        }
        shim_strcpy(resolved, sizeof(resolved), p);
    } else if (shim_excluded(p) || shim_strncmp(p, "/system", 7) == 0 ||
        shim_strncmp(p, "/vendor", 7) == 0 || shim_strncmp(p, "/apex", 5) == 0 ||
        shim_strncmp(p, "/product", 8) == 0 || shim_strncmp(p, "/odm", 4) == 0) {
        /* glibc ELF mimo ROOTFS (typicky uv managed Python pod /data/...):
         * primy bionicky execve by selhal (chybi /lib/ld-linux-aarch64.so.1),
         * takze ho posleme pres loader. Cesta se NEPREKLADA pod ROOTFS
         * (binarka tam neni), preda se absolutni device cesta. Whitelist gate
         * se pro tento pripad preskakuje (neni to guest binarka v ROOTFS). */
        if (p[0] == '/' && is_glibc_elf(p)) {
            shim_strcpy(resolved, sizeof(resolved), p);
            force_redirect = 1;
        } else {
            /* Excluded / host binaries -> real execve */
            fp_execve f = (fp_execve)g_orig_execve;
            return f ? f(p, argv, envp) : -1;
        }
    } else if (p[0] == '/') {
        /* Absolute guest path, e.g. /bin/ls or /usr/bin/gcc */
        if (g_f2_active && shim_translate(p, resolved, sizeof(resolved))) {
            /* shim translated */
        } else if (rl) {
            shim_strcpy(resolved, sizeof(resolved), g_shim_root);
            shim_strcat(resolved, sizeof(resolved), p);
        } else {
            shim_strcpy(resolved, sizeof(resolved), p);
        }
    } else {
        /* Relative path or bare command name */
        if (shim_strchr(p, '/')) {
            if (rl) {
                char cwd[1024];
                if (raw_getcwd(cwd, sizeof(cwd))) {
                    if (shim_strncmp(cwd, g_shim_root, rl) == 0) {
                        shim_strcpy(resolved, sizeof(resolved), cwd);
                        shim_strcat(resolved, sizeof(resolved), "/");
                        shim_strcat(resolved, sizeof(resolved), p);
                    } else {
                        shim_strcpy(resolved, sizeof(resolved), g_shim_root);
                        shim_strcat(resolved, sizeof(resolved), cwd);
                        shim_strcat(resolved, sizeof(resolved), "/");
                        shim_strcat(resolved, sizeof(resolved), p);
                    }
                } else {
                    shim_strcpy(resolved, sizeof(resolved), g_shim_root);
                    shim_strcat(resolved, sizeof(resolved), "/");
                    shim_strcat(resolved, sizeof(resolved), p);
                }
            } else {
                shim_strcpy(resolved, sizeof(resolved), p);
            }
        } else {
            /* Bare name, e.g. "ls" or "bash" -> Search guest PATH */
            if (!search_guest_path(p, envp, resolved, sizeof(resolved))) {
                shim_strcpy(resolved, sizeof(resolved), p);
            }
        }
    }

    /* Check for Shebang (#!) in resolved file */
    char interp[8192];
    interp[0] = 0;
    char interp_arg[8192];
    interp_arg[0] = 0;
    int is_script = 0;

    const char *chkpath = resolved[0] ? resolved : p;
    int fd = raw_open(chkpath, O_RDONLY);
    if (fd >= 0) {
        char header[256];
        ssize_t n = raw_read(fd, header, sizeof(header) - 1);
        raw_close(fd);
        if (n >= 2 && header[0] == '#' && header[1] == '!') {
            header[n] = 0;
            char *line = header + 2;
            while (*line == ' ' || *line == '\t') line++;
            char *eol = shim_strchr(line, '\n');
            if (eol) *eol = 0;
            char *eol2 = shim_strchr(line, '\r');
            if (eol2) *eol2 = 0;

            char *arg1 = line;
            while (*arg1 && *arg1 != ' ' && *arg1 != '\t') arg1++;
            if (*arg1) {
                *arg1 = 0;
                arg1++;
                while (*arg1 == ' ' || *arg1 == '\t') arg1++;
                if (*arg1) shim_strcpy(interp_arg, sizeof(interp_arg), arg1);
            }

            if (shim_strcmp(line, "/usr/bin/env") == 0 && interp_arg[0]) {
                if (!search_guest_path(interp_arg, envp, interp, sizeof(interp))) {
                    if (rl) {
                        shim_strcpy(interp, sizeof(interp), g_shim_root);
                        shim_strcat(interp, sizeof(interp), "/usr/bin/");
                        shim_strcat(interp, sizeof(interp), interp_arg);
                    } else {
                        shim_strcpy(interp, sizeof(interp), interp_arg);
                    }
                }
                interp_arg[0] = 0;
            } else if (rl && line[0] == '/') {
                shim_strcpy(interp, sizeof(interp), g_shim_root);
                shim_strcat(interp, sizeof(interp), line);
            } else {
                shim_strcpy(interp, sizeof(interp), line);
            }
            is_script = 1;
        }
    }

    /* Whitelist gate: binarky mimo whitelist se NEredirectuji (real execve),
     * ale zaloguji se do white.log, aby bylo videt co chybi. glibc ELF mimo
     * ROOTFS (force_redirect) gate obchazi - neni to guest binarka v ROOTFS. */
    if (!force_redirect && !wl_match(p)) {
        wl_log_skip(p, "not-in-whitelist");
        fp_execve rf = (fp_execve)g_orig_execve;
        return rf ? rf(p, argv, envp) : -1;
    }
    wl_log_exec(p, chkpath, is_script, interp);

    char *na[512];
    int narg = 0;
    const char *loader_bin = g_shim_loader && g_shim_loader[0] ? g_shim_loader : "/proc/self/exe";
    const char *mode_str = g_exec_mode ? g_exec_mode : "--ownall";

    na[narg++] = (char *)loader_bin;
    na[narg++] = (char *)mode_str;

    if (is_script && interp[0]) {
        na[narg++] = interp;
        if (interp_arg[0]) na[narg++] = interp_arg;
        na[narg++] = (char *)chkpath;
    } else {
        na[narg++] = (char *)chkpath;
    }

    for (int i = 1; argv && argv[i] && narg < 510; i++) {
        na[narg++] = argv[i];
    }
    na[narg] = NULL;

    fp_execve f = (fp_execve)g_orig_execve;
    char **cenv = shim_child_envp(envp, chkpath);
    int rr = f ? f(loader_bin, na, cenv) : -1;
    wl_log_fail(p, chkpath, rr);   /* sem se dostaneme jen kdyz execve selhal */
    return rr;
}

static int shim_execv(const char *p, char *const argv[]) {
    return shim_execve(p, argv, guest_environ());
}
static int shim_execvp(const char *p, char *const argv[]) {
    return shim_execve(p, argv, guest_environ());
}
static int shim_execvpe(const char *p, char *const argv[], char *const envp[]) {
    return shim_execve(p, argv, envp);
}

/* execl/execlp/execle: variadicke varianty. glibc je vola interne pres
 * __execve (mimo PLT), takze bez vlastniho shimu obchazely preklad cesty
 * a guest binarka dostala ENOENT na PT_INTERP. Prakticky: tmux spousti
 * shell panelu pres execl(shell, "-bash", NULL). */
#define SHIM_EXECL_MAX 1024
static int shim_execl(const char *p, const char *arg, ...) {
    char *av[SHIM_EXECL_MAX]; int n = 0; va_list ap;
    av[n++] = (char *)arg;
    va_start(ap, arg);
    while (arg && n < SHIM_EXECL_MAX - 1 && (av[n] = va_arg(ap, char *)) != NULL) n++;
    va_end(ap);
    av[n] = NULL;
    return shim_execve(p, av, guest_environ());
}
static int shim_execlp(const char *p, const char *arg, ...) {
    char *av[SHIM_EXECL_MAX]; int n = 0; va_list ap;
    av[n++] = (char *)arg;
    va_start(ap, arg);
    while (arg && n < SHIM_EXECL_MAX - 1 && (av[n] = va_arg(ap, char *)) != NULL) n++;
    va_end(ap);
    av[n] = NULL;
    return shim_execve(p, av, guest_environ());
}
static int shim_execle(const char *p, const char *arg, ...) {
    char *av[SHIM_EXECL_MAX]; int n = 0; va_list ap;
    av[n++] = (char *)arg;
    va_start(ap, arg);
    if (arg) {
        while (n < SHIM_EXECL_MAX - 1 && (av[n] = va_arg(ap, char *)) != NULL) n++;
        if (n == SHIM_EXECL_MAX - 1)   /* dotahni zbytek az po NULL */
            while (va_arg(ap, char *) != NULL) ;
    }
    char *const *envp = va_arg(ap, char *const *);
    va_end(ap);
    av[n] = NULL;
    return shim_execve(p, av, envp);
}

#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static int shim_execveat(int dirfd, const char *p, char *const argv[], char *const envp[], int flags) {
    if ((!p || !p[0]) && (flags & AT_EMPTY_PATH) && dirfd >= 0) {
        char fpath[64], target[8192];
        /* build /proc/self/fd/<N> without snprintf */
        shim_strcpy(fpath, sizeof(fpath), "/proc/self/fd/");
        char numbuf[16]; int ni = 0;
        int tmp = dirfd;
        if (tmp == 0) { numbuf[ni++] = '0'; }
        else { char rev[16]; int ri = 0; while (tmp > 0) { rev[ri++] = '0' + tmp % 10; tmp /= 10; } while (ri > 0) numbuf[ni++] = rev[--ri]; }
        numbuf[ni] = 0;
        shim_strcat(fpath, sizeof(fpath), numbuf);
        ssize_t n = raw_readlinkat(fpath, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            return shim_execve(target, argv, envp);
        }
    }
    if (dirfd != AT_FDCWD && dirfd >= 0 && p && p[0] != '/') {
        char fpath[64], dtarget[8192], combined[8192];
        shim_strcpy(fpath, sizeof(fpath), "/proc/self/fd/");
        char numbuf[16]; int ni = 0;
        int tmp = dirfd;
        if (tmp == 0) { numbuf[ni++] = '0'; }
        else { char rev[16]; int ri = 0; while (tmp > 0) { rev[ri++] = '0' + tmp % 10; tmp /= 10; } while (ri > 0) numbuf[ni++] = rev[--ri]; }
        numbuf[ni] = 0;
        shim_strcat(fpath, sizeof(fpath), numbuf);
        ssize_t n = raw_readlinkat(fpath, dtarget, sizeof(dtarget) - 1);
        if (n > 0) {
            dtarget[n] = '\0';
            shim_strcpy(combined, sizeof(combined), dtarget);
            shim_strcat(combined, sizeof(combined), "/");
            shim_strcat(combined, sizeof(combined), p);
            return shim_execve(combined, argv, envp);
        }
    }
    return shim_execve(p, argv, envp);
}

typedef void *(*fp_opendir)(const char *);
typedef ssize_t (*fp_readlink)(const char *, char *, size_t);
typedef ssize_t (*fp_readlinkat)(int, const char *, char *, size_t);
typedef char *(*fp_realpath)(const char *, char *);
typedef void *(*fp_dlopen)(const char *, int);
static void *g_orig_dlsym;
static void *g_orig_dlclose;
static void *g_orig_dlerror;
static void *g_orig_dladdr;
typedef int (*fp_chdir)(const char *);
static void *shim_opendir(const char *p) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_opendir f = (fp_opendir)g_orig_opendir; return f ? f(path) : NULL;
}
static ssize_t shim_readlink(const char *p, char *b, size_t n) {
    char x[8192]; const char *path = p; if (shim_translate(p, x, sizeof x)) path = x;
    fp_readlink f = (fp_readlink)g_orig_readlink; return f ? f(path, b, n) : -1;
}
static ssize_t shim_readlinkat(int d, const char *p, char *b, size_t n) {
    char x[8192]; const char *path = p;
    if (d == -100 && p && p[0] == '/') { if (shim_translate(p, x, sizeof x)) path = x; }
    fp_readlinkat f = (fp_readlinkat)g_orig_readlinkat; return f ? f(d, path, b, n) : -1;
}
/* Guest-aware kanonizace: glibc realpath prochazi komponenty a absolutni cil
 * symlinku (venv: .venv/bin/python -> /bin/python3) vyhodnoti proti HOST
 * rootu (/bin -> /system/bin) -> ENOENT; uv pak venv povazuje za rozbity.
 * Tady symlinky resime sami: absolutni cile pod ROOTFS, .l2s cile (uz pod
 * ROOTFS) beze zmeny. Vysledek = hostova cesta bez symlinku (glibc realpath
 * nad ni uz nic nepreklada a jen alokuje). Vraci 1 = host_out platny,
 * 0 = neresit (mimo ROOTFS / exclude), -1 = chyba (errno v *err). */
static void shim_guest_errno_set(int v);
static int shim_guest_canon(const char *p, char *host_out, size_t n, int *err) {
    if (!p || !p[0] || !g_shim_root || !g_shim_root[0]) return 0;
    size_t rl = shim_strlen(g_shim_root);
    char g[8192];                        /* guest cesta ke zpracovani */
    if (p[0] == '/') {
        if (shim_strncmp(p, g_shim_root, rl) == 0 && (p[rl] == '/' || p[rl] == 0))
            shim_strcpy(g, sizeof g, p[rl] ? p + rl : "/");
        else if (shim_excluded(p)) return 0;
        else shim_strcpy(g, sizeof g, p);
    } else {
        char cwd[4096];
        if (!raw_getcwd(cwd, sizeof cwd)) return 0;
        if (!(shim_strncmp(cwd, g_shim_root, rl) == 0 && (cwd[rl] == '/' || cwd[rl] == 0)))
            return 0;                     /* cwd mimo ROOTFS -> glibc */
        shim_strcpy(g, sizeof g, cwd[rl] ? cwd + rl : "/");
        shim_strcat(g, sizeof g, "/");
        shim_strcat(g, sizeof g, p);
    }
    char out[8192];                      /* vyreseny guest prefix */
    out[0] = 0;
    char rest[8192];
    shim_strcpy(rest, sizeof rest, g);
    int links = 0;
    const char *r = rest;
    while (*r) {
        while (*r == '/') r++;
        if (!*r) break;
        const char *e = r;
        while (*e && *e != '/') e++;
        size_t cl = (size_t)(e - r);
        if (cl == 1 && r[0] == '.') { r = e; continue; }
        if (cl == 2 && r[0] == '.' && r[1] == '.') {
            size_t ol = shim_strlen(out);
            while (ol > 0 && out[ol - 1] != '/') ol--;
            if (ol > 0) ol--;
            out[ol] = 0;
            r = e; continue;
        }
        size_t ol = shim_strlen(out);
        if (ol + 1 + cl + 1 > sizeof out) { *err = 36; return -1; }
        out[ol] = '/';
        shim_memcpy(out + ol + 1, r, cl);
        out[ol + 1 + cl] = 0;
        if (ol == 0 && shim_excluded(out)) {
            /* host strom (/proc, /data...): zbytek resi glibc */
            shim_strcat(out, sizeof out, e);
            if (shim_strlen(out) + 1 > n) { *err = 36; return -1; }
            shim_strcpy(host_out, n, out);
            return 1;
        }
        char h[8192];
        shim_strcpy(h, sizeof h, g_shim_root);
        shim_strcat(h, sizeof h, out);
        unsigned char st[256] __attribute__((aligned(16)));
        long sr = shim_raw_syscall6(79, -100, (long)h, (long)st, 0x100, 0, 0);
        if (sr != 0) { *err = (int)-sr; return -1; }
        unsigned int mode = *(unsigned int *)(st + 16);
        if ((mode & 0170000) == 0120000) {
            if (++links > 40) { *err = 40; return -1; }
            char lb[4096];
            long lr = shim_raw_syscall6(78, -100, (long)h, (long)lb, sizeof lb - 1, 0, 0);
            if (lr <= 0) { *err = lr < 0 ? (int)-lr : 2; return -1; }
            lb[lr] = 0;
            char nr[8192];
            const char *tgt = lb;
            if (lb[0] == '/' && shim_strncmp(lb, g_shim_root, rl) == 0 &&
                (lb[rl] == '/' || lb[rl] == 0))
                tgt = lb[rl] ? lb + rl : "/";          /* .l2s: hostova abs. */
            if (tgt[0] == '/') {
                out[0] = 0;                              /* absolutni: od koren */
            } else {
                size_t k = shim_strlen(out);             /* relativni: vuci adresari */
                while (k > 0 && out[k - 1] != '/') k--;
                if (k > 0) k--;
                out[k] = 0;
            }
            shim_strcpy(nr, sizeof nr, tgt);
            shim_strcat(nr, sizeof nr, e);
            shim_strcpy(rest, sizeof rest, nr);
            r = rest;
            continue;
        }
        if (*e && (mode & 0170000) != 0040000) { *err = 20; return -1; }  /* ENOTDIR */
        r = e;
    }
    if (!out[0]) shim_strcpy(out, sizeof out, "/");
    if (rl + shim_strlen(out) + 1 > n) { *err = 36; return -1; }
    shim_strcpy(host_out, n, g_shim_root);
    if (!(out[0] == '/' && out[1] == 0)) shim_strcat(host_out, n, out);
    return 1;
}
/* Spolecny vstup pro realpath rodinu: 1 = path prelozena/vyresena,
 * -1 = chyba (errno uz nastaveno, vrat NULL). */
static int shim_canon_path(const char *p, char *buf, size_t n, const char **path) {
    int err = 0;
    int r = shim_guest_canon(p, buf, n, &err);
    if (r < 0) { shim_guest_errno_set(err); return -1; }
    if (r > 0) { *path = buf; return 1; }
    if (shim_translate(p, buf, n)) { *path = buf; return 1; }
    return 0;
}

/* Vysledek realpath pod ROOTFS -> guest pohled (/usr/...), aby program videl
 * stejne cesty jako v proot; pri dalsim pouziti projdou beznym prekladem. */
static char *shim_strip_root(char *r) {
    if (!r || !g_shim_root || !g_shim_root[0]) return r;
    size_t rl = shim_strlen(g_shim_root);
    if (shim_strncmp(r, g_shim_root, rl) != 0) return r;
    if (r[rl] == 0) { r[0] = '/'; r[1] = 0; return r; }
    if (r[rl] != '/') return r;
    size_t i = 0;
    do { r[i] = r[rl + i]; } while (r[i++]);
    return r;
}
static char *shim_realpath(const char *p, char *b) {
    char x[8192]; const char *path = p; if (shim_canon_path(p, x, sizeof x, &path) < 0) return NULL;
    fp_realpath f = (fp_realpath)g_orig_realpath;
    return shim_strip_root(f ? f(path, b) : NULL);
}
static void *g_real_canonicalize, *g_real_realpath_chk;
static char *shim_canonicalize_file_name(const char *p) {
    char x[8192]; const char *path = p; if (shim_canon_path(p, x, sizeof x, &path) < 0) return NULL;
    char *(*f)(const char *) = (char *(*)(const char *))g_real_canonicalize;
    return shim_strip_root(f ? f(path) : NULL);
}
static char *shim_realpath_chk(const char *p, char *b, size_t n) {
    char x[8192]; const char *path = p; if (shim_canon_path(p, x, sizeof x, &path) < 0) return NULL;
    char *(*f)(const char *, char *, size_t) = (char *(*)(const char *, char *, size_t))g_real_realpath_chk;
    return shim_strip_root(f ? f(path, b, n) : NULL);
}
static void *shim_dlopen(const char *p, int f) {
    char b[8192]; const char *path = p;
    if (p && p[0] == '/') {
        if (shim_translate(p, b, sizeof b)) path = b;
    } else if (p && shim_strchr(p, '/')) {
        if (g_shim_root && g_shim_root[0]) {
            shim_strcpy(b, sizeof(b), g_shim_root);
            shim_strcat(b, sizeof(b), "/usr/lib/aarch64-linux-gnu/");
            shim_strcat(b, sizeof(b), p);
            if (raw_access(b, F_OK) == 0) {
                path = b;
            } else {
                shim_strcpy(b, sizeof(b), g_shim_root);
                shim_strcat(b, sizeof(b), "/lib/aarch64-linux-gnu/");
                shim_strcat(b, sizeof(b), p);
                if (raw_access(b, F_OK) == 0) {
                    path = b;
                } else {
                    shim_strcpy(b, sizeof(b), g_shim_root);
                    shim_strcat(b, sizeof(b), "/");
                    shim_strcat(b, sizeof(b), p);
                    if (raw_access(b, F_OK) == 0) path = b;
                }
            }
        }
    }
    /* --ownall: guest glibc dlopen padá na nulovém _rtld_global -> naše
     * náhrada nad scopes. Non-ownall (host bionic): reálný dlopen. */
    if (elf_own_deps)
        return ldso_dlopen(path, f);
    fp_dlopen ff = (fp_dlopen)g_orig_dlopen;
    return ff ? ff(path, f) : NULL;
}
static void *shim_dlsym(void *h, const char *name) {
    if (elf_own_deps)
        return ldso_dlsym(h, name);
    void *(*ff)(void *, const char *) = (void *(*)(void *, const char *))g_orig_dlsym;
    return ff ? ff(h, name) : NULL;
}
static int shim_dlclose(void *h) {
    if (elf_own_deps)
        return ldso_dlclose(h);
    int (*ff)(void *) = (int (*)(void *))g_orig_dlclose;
    return ff ? ff(h) : 0;
}
static const char *shim_dlerror(void) {
    if (elf_own_deps)
        return ldso_dlerror();
    const char *(*ff)(void) = (const char *(*)(void))g_orig_dlerror;
    return ff ? ff() : NULL;
}
typedef int (*fp_posix_spawnp)(pid_t *, const char *,
                               const void *, const void *,
                               char *const[], char *const[]);
static void *g_orig_posix_spawnp;
static void *g_orig_posix_spawn;

/* uv používá posix_spawnp (Rust std) pro zjištění libc: spouští
 * /lib/ld-linux-aarch64.so.1 --version. Guest cesta se musí přeložit na
 * device a re-exec přes elf_loader (jinak kernel hledá /lib/ld-linux na
 * hostu -> ENOENT -> "Could not detect libc"). Delegujeme na reálný guest
 * glibc posix_spawnp s argv = [elf_loader, --ownall, resolved, ...]. */
static int shim_posix_spawnp(pid_t *pid, const char *p, const void *fa,
                             const void *at, char *const argv[],
                             char *const envp[]) {
    if (!p || !p[0]) return -1;
    char resolved[8192];
    resolved[0] = 0;
    size_t rl = g_shim_root ? shim_strlen(g_shim_root) : 0;
    if (p[0] == '/' && rl && shim_strncmp(p, g_shim_root, rl) == 0 &&
        (p[rl] == '/' || p[rl] == 0)) {
        /* Uz pod ROOTFS (napr. uv spousti $ROOTFS/.../.venv/bin/python):
         * MUSI byt pred exclude testem - ROOTFS lezi pod /data, takze by
         * jinak sel real posix_spawn bez loaderu -> exit 127. Rootfs ale
         * muze obsahovat i bionic-staticky nastroj (napr. "ashell" v
         * usr/local/bin) - ten se MA spustit primo, jinak ho loader zkousi
         * own-loadovat jako glibc a spadne hned na vstupu (shodna oprava
         * jako v shim_execve). */
        unsigned char emag[4] = {0, 0, 0, 0};
        int efd = raw_open(p, O_RDONLY);
        if (efd >= 0) { raw_read(efd, emag, 4); raw_close(efd); }
        if (emag[0] == 0x7f && emag[1] == 'E' && emag[2] == 'L' && emag[3] == 'F' &&
            !is_glibc_elf(p)) {
            fp_posix_spawnp f = (fp_posix_spawnp)g_orig_posix_spawnp;
            return f ? f(pid, p, fa, at, argv, envp) : -1;
        }
        shim_strcpy(resolved, sizeof resolved, p);
    } else if (p[0] == '/') {
        /* Excluded host cesty (/system, /vendor, /apex, /proc, ...) se
         * NESMI prekladat pod ROOTFS a NESMI se re-execovat pres loader.
         * Musi se spustit primo (real posix_spawnp), jinak loader pokusi
         * nacist bionicky binarku jako guest glibc a spadne na libc.so.
         * (shim_execve to dela spravne, tady chybelo early return). */
        if (shim_excluded(p) || shim_strncmp(p, "/system", 7) == 0 ||
            shim_strncmp(p, "/vendor", 7) == 0 || shim_strncmp(p, "/apex", 5) == 0 ||
            shim_strncmp(p, "/product", 8) == 0 || shim_strncmp(p, "/odm", 4) == 0) {
            /* glibc ELF mimo ROOTFS (uv managed Python pod /data/...): pres
             * loader, cesta zustava absolutni device cesta (neprekladat). */
            if (is_glibc_elf(p)) {
                shim_strcpy(resolved, sizeof resolved, p);
            } else {
                fp_posix_spawnp f = (fp_posix_spawnp)g_orig_posix_spawnp;
                return f ? f(pid, p, fa, at, argv, envp) : -1;
            }
        } else if (!shim_translate(p, resolved, sizeof resolved)) {
            if (rl && shim_strncmp(p, g_shim_root, rl) == 0)
                shim_strcpy(resolved, sizeof resolved, p);
            else if (rl) {
                shim_strcpy(resolved, sizeof resolved, g_shim_root);
                shim_strcat(resolved, sizeof resolved, p);
            } else {
                shim_strcpy(resolved, sizeof resolved, p);
            }
        }
    } else if (shim_strchr(p, '/')) {
        /* relativni cesta (./x, bin/x) je vuci cwd, ne vuci koreni ROOTFS */
        char cwd[4096];
        if (raw_getcwd(cwd, sizeof cwd)) {
            if (rl && !(shim_strncmp(cwd, g_shim_root, rl) == 0 &&
                        (cwd[rl] == '/' || cwd[rl] == 0)) && !shim_excluded(cwd)) {
                shim_strcpy(resolved, sizeof resolved, g_shim_root);
                shim_strcat(resolved, sizeof resolved, cwd);
            } else {
                shim_strcpy(resolved, sizeof resolved, cwd);
            }
            shim_strcat(resolved, sizeof resolved, "/");
            shim_strcat(resolved, sizeof resolved, p);
        } else {
            shim_strcpy(resolved, sizeof resolved, p);
        }
    } else {
        if (!search_guest_path(p, envp, resolved, sizeof resolved)) {
            if (rl) {
                shim_strcpy(resolved, sizeof resolved, g_shim_root);
                shim_strcat(resolved, sizeof resolved, "/usr/bin/");
                shim_strcat(resolved, sizeof resolved, p);
            } else {
                shim_strcpy(resolved, sizeof resolved, p);
            }
        }
    }
    if (!resolved[0]) return -1;

    const char *loader_bin = g_shim_loader && g_shim_loader[0]
                                 ? g_shim_loader : "/proc/self/exe";
    char *na[512];
    int narg = 0;
    na[narg++] = (char *)loader_bin;
    na[narg++] = (char *)(g_exec_mode ? g_exec_mode : "--ownall");
    na[narg++] = resolved;
    for (int i = 1; argv && argv[i] && narg < 510; i++)
        na[narg++] = argv[i];
    na[narg] = NULL;

    fp_posix_spawnp f = (fp_posix_spawnp)g_orig_posix_spawnp;
    char **cenv = shim_child_envp(envp, resolved);
    return f ? f(pid, loader_bin, fa, at, na, cenv) : -1;
}

static int shim_dladdr(const void *addr, void *info) {
    if (elf_own_deps)
        return ldso_dladdr(addr, info);
    int (*ff)(const void *, void *) = (int (*)(const void *, void *))g_orig_dladdr;
    return ff ? ff(addr, info) : 0;
}
static int shim_chdir(const char *p) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_chdir f = (fp_chdir)g_orig_chdir; return f ? f(path) : -1;
}

static FILE *shim_fopen(const char *p, const char *mode) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    FILE *(*f)(const char *, const char *) = (FILE *(*)(const char *, const char *))g_orig_fopen;
    return f ? f(path, mode) : NULL;
}
static FILE *shim_fopen64(const char *p, const char *mode) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    char resolved[8192];
    if (shim_resolve_symlinks(path, resolved, sizeof(resolved))) path = resolved;
    FILE *(*f)(const char *, const char *) = (FILE *(*)(const char *, const char *))g_orig_fopen64;
    return f ? f(path, mode) : NULL;
}
static int shim___xstat64(int v, const char *p, void *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_xstat f = (fp_xstat)g_orig___xstat64; return f ? f(v, path, st) : -1;
}
static int shim___lxstat64(int v, const char *p, void *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_lxstat f = (fp_lxstat)g_orig___lxstat64; return f ? f(v, path, st) : -1;
}
/* __fxstatat64 / __fxstatat: STARY ABI s verzi jako PRVNIM argumentem:
 *   int __fxstatat64(int ver, int fd, const char *file, struct stat64 *buf, int flag);
 *   int __fxstatat  (int ver, int fd, const char *file, struct stat  *buf, int flag);
 * Glibc fstatat64() -> __fxstatat64(_STAT_VER, fd, path, buf, flags). Puvodni
 * 4-arg shim (dfd, p, st, flags) interpretoval 'fd' (cislo) jako ukazatel na
 * cestu -> EFAULT ("Bad address") u KAZDEHO statu s dir_fd: Python
 * os.stat(dir_fd=...), shutil.rmtree, PEP517 build wheel (bdist_wheel
 * kopiruje *.dist-info pres DirEntry.stat) -> pad buildu python-nmap apod.
 * (Zavazne: 'ver' musi byt prvni, 'fd' druhy.) */
typedef int (*fp_fxstatat)(int, int, const char *, void *, int);
static int shim___fxstatat64(int ver, int dfd, const char *p, void *st, int flags) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_fxstatat f = (fp_fxstatat)g_orig___fxstatat64;
    return f ? f(ver, dfd, path, st, flags) : -1;
}
static int shim___fxstatat(int ver, int dfd, const char *p, void *st, int flags) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_fxstatat f = (fp_fxstatat)g_orig___fxstatat;
    return f ? f(ver, dfd, path, st, flags) : -1;
}
static int shim_faccessat2(int dfd, const char *p, int m, int ff) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_faccessat f = (fp_faccessat)g_orig_faccessat2; return f ? f(dfd, path, m, ff) : -1;
}
typedef int (*fp_getrlimit)(int, struct rlimit *);
static int shim_getrlimit(int resource, struct rlimit *rl) {
    fp_getrlimit f = (fp_getrlimit)g_orig_getrlimit;
    int res = f ? f(resource, rl) : -1;
    if (res == 0 && resource == 3 /* RLIMIT_STACK */ && rl) {
        if (rl->rlim_cur == 0 || rl->rlim_cur == RLIM_INFINITY || rl->rlim_cur < 8 * 1024 * 1024) {
            rl->rlim_cur = 8 * 1024 * 1024;
        }
    }
    return res;
}

typedef int (*fp_fileno_unlocked)(FILE *);
typedef int (*fp_fileno)(FILE *);

/* Forward declarations for shim_mprotect */
static void shim_hex(char **pp, unsigned long v, int nib);
static int *shim_guest_errno(void);

typedef int (*fp_mprotect)(void *, unsigned long, int);
static int shim_mprotect(void *addr, unsigned long len, int prot) {
    fp_mprotect f = (fp_mprotect)g_orig_mprotect;
    int r = f ? f(addr, len, prot) : -1;
    if (g_vmtrace) {
        char b[128]; char *i = b;
        const char *p = "[MPROT] addr="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)addr, 12);
        p = " len="; while (*p) *i++ = *p++;
        shim_hex(&i, len, 8);
        p = " prot="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)(unsigned)prot, 2);
        p = " rc="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)(unsigned)r, 2);
        *i++ = '\n';
        shim_raw_syscall6(64, 2, (long)b, (long)(i - b), 0, 0, 0);
    }
    if (r != 0) {
        char b[128]; char *i = b;
        const char *p = "[MPROT_FAIL] addr="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)addr, 12);
        p = " len="; while (*p) *i++ = *p++;
        shim_hex(&i, len, 8);
        p = " prot="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)(unsigned)prot, 2);
        p = " err="; while (*p) *i++ = *p++;
        int err = 0; int *e = shim_guest_errno(); if (e) err = *e;
        shim_hex(&i, (unsigned long)(unsigned)err, 2);
        *i++ = '\n';
        shim_raw_syscall6(64, 2, (long)b, (long)(i - b), 0, 0, 0);
    }
    return r;
}
typedef void (*fp_flockfile)(FILE *);

static int shim_fileno_unlocked(FILE *fp) {
    if (!fp) return -1;   /* guest TP: zadny bionic errno/fprintf */
    fp_fileno_unlocked f = (fp_fileno_unlocked)g_orig_fileno_unlocked;
    return f ? f(fp) : -1;
}

static int shim_fileno(FILE *fp) {
    /* bezi pod guest TP: zadny bionic errno/fprintf */
    if (!fp) return -1;
    fp_fileno f = (fp_fileno)g_orig_fileno;
    return f ? f(fp) : -1;
}

__attribute__((unused)) static void shim_flockfile(FILE *fp) {
    if (elf_debug()) fprintf(stderr, "[dbg] flockfile called with fp=%p\n", fp);
    fp_flockfile f = (fp_flockfile)g_orig_flockfile;
    if (f) f(fp);
}

/* DIAG: __assert_fail override - zachyti assertion a vypise retezec volajicich
 * pres __builtin_return_address. Node/libuv v uv__close assertuje pri fd<=2;
 * potřebujeme zjistit, KDO uv__close vola. Zapisy raw write(2) - bez TLS. */
static void raw_wr2(const char *b, int n) {
    register long x8 __asm__("x8") = 64;
    register long x0 __asm__("x0") = 2;
    register long x1 __asm__("x1") = (long)b;
    register long x2 __asm__("x2") = n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
}
static void hexcat(char *b, int *i, unsigned long v) {
    static const char hx[] = "0123456789abcdef";
    b[(*i)++] = '0'; b[(*i)++] = 'x';
    int started = 0;
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)((v >> sh) & 0xf);
        if (d || started || sh == 0) { b[(*i)++] = hx[d]; started = 1; }
    }
}
static void (*g_orig_assert_fail)(const char*, const char*, unsigned int, const char*);
static void shim_assert_fail(const char *assertion, const char *file,
                             unsigned int line, const char *function) {
    char b[512]; int i = 0;
    const char *p = "ASSERT ra0=";
    while (*p) b[i++] = *p++;
    hexcat(b, &i, (unsigned long)__builtin_return_address(0));
    p = " ra1="; while (*p) b[i++] = *p++;
    hexcat(b, &i, (unsigned long)__builtin_return_address(1));
    p = " ra2="; while (*p) b[i++] = *p++;
    hexcat(b, &i, (unsigned long)__builtin_return_address(2));
    p = " ra3="; while (*p) b[i++] = *p++;
    hexcat(b, &i, (unsigned long)__builtin_return_address(3));
    p = " line="; while (*p) b[i++] = *p++;
    { unsigned int v = line; char t[12]; int ti = 0;
      if (!v) t[ti++] = '0';
      while (v) { t[ti++] = (char)('0' + (v % 10)); v /= 10; }
      while (ti) b[i++] = t[--ti]; }
    b[i++] = '\n';
    raw_wr2(b, i);
    if (file) { char fb[256]; int j = 0; const char *q = file;
                while (*q && j < 250) fb[j++] = *q++;
                fb[j++] = '\n'; raw_wr2(fb, j); }
    if (!g_orig_assert_fail && g_shim_scope)
        g_orig_assert_fail = (void *)elf_scope_lookup(g_shim_scope, "__assert_fail");
    if (g_orig_assert_fail)
        g_orig_assert_fail(assertion, file, line, function);
    _exit(134);
}

/* V8/node volá pthread_getattr_np(main) a testuje, ze SP lezi ve vracenem
 * rozsahu (IsOnCentralStack). Náš guest bezi na mmap stacku (8 MB), ktery
 * ale v /proc/self/maps NENI oznaceny jako [stack] - glibc proto vrati
 * ENOENT. Override: zavolej orig, pri chybe napln attr nasim stackem.
 * attr layout (glibc aarch64): +0x10 guardsize, +0x18 stack TOP, +0x20 size. */
#define ATTR_STACKADDR 0x18
#define ATTR_STACKSIZE 0x20
typedef int (*fp_pthread_getattr_np)(void *th, void *attr);
static int shim_pthread_getattr_np(void *th, void *attr) {
    fp_pthread_getattr_np f = (fp_pthread_getattr_np)elf_scope_lookup(g_shim_scope, "pthread_getattr_np");
    int r = f ? f(th, attr) : -1;
    if (r != 0 && attr) {
        extern uintptr_t g_guest_stack_base, g_guest_stack_size;
        if (g_guest_stack_size) {
            unsigned char *a = (unsigned char *)attr;
            *(unsigned long *)(a + ATTR_STACKADDR) =
                g_guest_stack_base + g_guest_stack_size;
            *(unsigned long *)(a + ATTR_STACKSIZE) = g_guest_stack_size;
            r = 0;
        }
    }
    return r;
}


/* DIAG: pthread_create override - pri EINVAL vypise pole attr (stacksize,
 * guardsize, flags). V8/node vytvari vlakna s vlastnimi atributy; EINVAL
 * znamena, ze nejaka hodnota neprosla glibc validaci. */
typedef int (*fp_pthread_create)(void*, const void*, void *(*)(void*), void*);
static int shim_pthread_create(void *th, const void *attr, void *(*fn)(void*), void *arg) {
    fp_pthread_create f = (fp_pthread_create)elf_scope_lookup(g_shim_scope, "pthread_create");
    if (!f) return 22;
    int r = f(th, attr, fn, arg);
    /* Nase dl_tls_static_size (~1 MB) zvetsuje __pthread_get_minstack, takze
     * glibc odmitne maly stacksize (node/V8 zadá 0x20000) -> EINVAL.
     * V realnem glibc je TLS static size maly a 128 KB staci. Retry s 8 MB.
     * attr layout (aarch64 glibc): flags+0, guardsize+0x10, stacksize+0x20. */
    if (r == 22 && attr) {
        unsigned char copy[64];
        for (int k = 0; k < 64; k++) copy[k] = ((const unsigned char *)attr)[k];
        *(unsigned long *)(copy + 0x20) = 8UL * 1024 * 1024;
        r = f(th, copy, fn, arg);
    }
    if (r != 0 && attr) {
        const unsigned char *a = (const unsigned char *)attr;
        static const char hxd[] = "0123456789abcdef";
        char b[512]; int i = 0;
        const char *p = "PTHCREATE rc=";
        while (*p) b[i++] = *p++;
        char t[12]; int ti = 0; int v = r;
        if (!v) t[ti++] = '0';
        while (v) { t[ti++] = (char)('0' + (v % 10)); v /= 10; }
        while (ti) b[i++] = t[--ti];
        for (int off = 0; off < 64; off += 8) {
            b[i++] = ' ';
            b[i++] = hxd[(off >> 4) & 0xf];
            b[i++] = hxd[off & 0xf];
            b[i++] = '=';
            unsigned long u = 0;
            for (int k = 7; k >= 0; k--) u = (u << 8) | a[off + k];
            for (int sh = 60; sh >= 0; sh -= 4) b[i++] = hxd[(u >> sh) & 0xf];
        }
        b[i++] = 10;
        raw_wr2(b, i);
    }
    return r;
}



typedef int (*fp_close)(int);
__attribute__((unused)) static int shim_close(int fd) {
    if (fd <= 2) {
        char b[80]; int i = 0;
        const char *p = "[CLOSE] fd=";
        while (*p) b[i++] = *p++;
        int v = fd; if (v < 0) { b[i++] = '-'; v = -v; }
        if (v >= 10) b[i++] = (char)('0' + (v / 10));
        b[i++] = (char)('0' + (v % 10));
        b[i++] = 10;
        register long x8 __asm__("x8") = 64;
        register long x0 __asm__("x0") = 2;
        register long x1 __asm__("x1") = (long)b;
        register long x2 __asm__("x2") = i;
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    }
    fp_close f = (fp_close)g_orig_close;
    return f ? f(fd) : -1;
}

/* App seccomp KILLuje setfsuid(151)/setfsgid(152) (KILL obchazi SIGSYS
 * handler). glibc/libtinfo je volaji (napr. _nc_safe_fopen pred fopen, aby
 * docasne zmenily fsuid). Na Androidu nejsou potreba (app uid je fsuid) ->
 * predstirejme uspech a vrat uid/gid bez zmeny. */
static long main_raw_syscall4(long nr, long a0, long a1, long a2, long a3);
/* raw getuid(174)/getgid(176): bionic getuid() pod guest TP nelze volat */
static int shim_setfsuid(unsigned int uid) { (void)uid; return (int)main_raw_syscall4(174, 0, 0, 0, 0); }
static int shim_setfsgid(unsigned int gid) { (void)gid; return (int)main_raw_syscall4(176, 0, 0, 0, 0); }

/* ===== MAP_FIXED_NOREPLACE emulace (kernel 4.14) =====
 * Android kernel 4.14 flag MAP_FIXED_NOREPLACE (0x100000) NEZNA -> ignoruje
 * ho a chova se jako hint. V8/Node s nim rezervuje 4GB pointer-compression
 * cage na PRESNE adrese (napr. 0x2853_0000_0000). Kdyz dostane jinou adresu
 * s errno=0, vsechny komprimovane pointery jsou divoke -> SIGSEGV v
 * Builtins_InterpreterEntryTrampoline (sturh wzr,[x5,#67] do r--p stranky).
 * Emulujeme spravnou semantiku: overime, ze rozsah je volny (parse
 * /proc/self/maps), a teprve pak pouzijeme MAP_FIXED (presna adresa).
 * Kdyz je obsazeny -> EEXIST (jako skutecny MAP_FIXED_NOREPLACE). */
#define SHIM_MAP_FIXED_NOREPLACE 0x100000UL
#define SHIM_MAP_FIXED           0x10UL

static int shim_addr_range_free(unsigned long start, unsigned long end) {
    register long x8 __asm__("x8") = 56;   /* openat */
    register long x0 __asm__("x0") = -100;
    register long x1 __asm__("x1") = (long)"/proc/self/maps";
    register long x2 __asm__("x2") = 0;
    register long x3 __asm__("x3") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8),"r"(x1),"r"(x2),"r"(x3)
                     : "memory","cc");
    int fd = (int)x0;
    if (fd < 0) return 1;                    /* optimisticky */
    static char mbuf[262144];
    long total = 0, n;
    while ((n = shim_raw_syscall6(63, fd, (long)(mbuf + total),
                                  sizeof mbuf - 1 - total, 0, 0, 0)) > 0) {
        total += n;
        if (total > (long)sizeof mbuf - 2) break;
    }
    shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
    if (total <= 0) return 1;
    mbuf[total] = 0;
    char *p = mbuf;
    while (*p) {
        unsigned long a = 0, b = 0;
        int any = 0;
        while (*p && *p != '-') {
            char c = *p++;
            if (c >= '0' && c <= '9') { a = a * 16 + (unsigned)(c - '0'); any = 1; }
            else if (c >= 'a' && c <= 'f') { a = a * 16 + (unsigned)(c - 'a' + 10); any = 1; }
        }
        if (*p == '-') p++;
        while (*p && *p != ' ') {
            char c = *p++;
            if (c >= '0' && c <= '9') b = b * 16 + (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') b = b * 16 + (unsigned)(c - 'a' + 10);
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (!any) continue;
        if (start < b && end > a) return 0;   /* prekryv -> obsazeno */
    }
    return 1;
}

typedef void *(*fp_mmap)(void *, unsigned long, int, int, int, long);
static void shim_hex(char **pp, unsigned long v, int nib) {
    static const char hxd[] = "0123456789abcdef";
    for (int sh = (nib - 1) * 4; sh >= 0; sh -= 4)
        *(*pp)++ = hxd[(v >> sh) & 0xf];
}
/* Diagnostika velkych mmap (V8 sandbox/cage rezervace): pred i po volani. */
static void shim_mmap_log(unsigned long len, void *addr, int flags,
                          void *res, int rerr, int prot) {
    char b[200]; char *i = b;
    const char *p = "[MMAP] len=";
    while (*p) *i++ = *p++;
    shim_hex(&i, len, 10);
    p = " addr="; while (*p) *i++ = *p++;
    shim_hex(&i, (unsigned long)addr, 12);
    p = " prot="; while (*p) *i++ = *p++;
    shim_hex(&i, (unsigned long)(unsigned)prot, 2);
    p = " fl="; while (*p) *i++ = *p++;
    shim_hex(&i, (unsigned long)(unsigned)flags, 6);
    p = " -> "; while (*p) *i++ = *p++;
    shim_hex(&i, (unsigned long)res, 12);
    p = " err="; while (*p) *i++ = *p++;
    shim_hex(&i, (unsigned long)(unsigned)rerr, 2);
    if (addr && res && res != (void *)-1)
        *i++ = (res == addr) ? 'M' : 'X';
    *i++ = '\n';
    shim_raw_syscall6(64, 2, (long)b, (long)(i - b), 0, 0, 0);
}
/* Guest (parrot glibc) errno. NESMIME sahat na bionicky `errno` - shim
 * bezi pod guest TP, kde bionicke __errno() cte TP+offset -> divoky
 * pointer (si_addr=0x300) a SIGSEGV v loaderu. Guest errno ziskame
 * z guest libc pres __errno_location. */
static int *shim_guest_errno(void) {
    int *(*f)(void) = (int *(*)(void))elf_scope_lookup(g_shim_scope, "__errno_location");
    if (f)
        return f();
    f = (int *(*)(void))elf_scope_lookup(g_shim_scope, "__errno_location64");
    return f ? f() : NULL;
}
static void shim_guest_errno_set(int v) {
    int *e = shim_guest_errno();
    if (e) *e = v;
}

static void *shim_mmap_common(void *addr, unsigned long len, int prot,
                              int flags, int fd, long off) {
    int dbg = g_vmtrace;   /* [MMAP] jen s ELF_LOADER_VMTRACE (jinak rusi TUI, napr. tmux) */
    if (addr && (flags & (int)SHIM_MAP_FIXED_NOREPLACE)) {
        unsigned long a = (unsigned long)addr;
        if (!shim_addr_range_free(a, a + len)) {
            shim_guest_errno_set(17);         /* EEXIST */
            if (dbg) shim_mmap_log(len, addr, flags, (void *)-1, 17, prot);
            return (void *)-1;
        }
        flags = (flags & ~(int)SHIM_MAP_FIXED_NOREPLACE) | (int)SHIM_MAP_FIXED;
    } else {
        flags = flags & ~(int)SHIM_MAP_FIXED_NOREPLACE;
    }
    fp_mmap f = (fp_mmap)elf_scope_lookup(g_shim_scope, "mmap");
    void *r; int e = 0;
    shim_guest_errno_set(0);
    if (!f) {
        /* fallback: raw mmap syscall (aarch64 222) */
        long raw = shim_raw_syscall6(222, (long)addr, (long)len, prot,
                                     flags, fd, off);
        if (raw < 0 && raw > -4096) {
            e = (int)-raw;
            shim_guest_errno_set(e);
            r = (void *)-1;
        } else {
            r = (void *)raw;
        }
    } else {
        r = f(addr, len, prot, flags, fd, off);
        int *ge = shim_guest_errno();
        e = ge ? *ge : 0;
    }
    if (dbg) shim_mmap_log(len, addr, flags, r, e, prot);
    return r;
}
static void *shim_mmap(void *a, unsigned long l, int p, int fl, int fd, long o) {
    return shim_mmap_common(a, l, p, fl, fd, o);
}
static void *shim_mmap64(void *a, unsigned long l, int p, int fl, int fd, long o) {
    return shim_mmap_common(a, l, p, fl, fd, o);
}
/* mprotect override pro --ownall (F2 ma vlastni shim_mprotect v g_f2_hooks). */
static int shim_mprotect_ov(void *addr, unsigned long len, int prot) {
    int eff = prot;
    if (g_ro_keep_write && (prot & PROT_READ)
        && !(prot & PROT_WRITE) && !(prot & PROT_EXEC))
        eff = prot | PROT_WRITE;
    fp_mprotect f = (fp_mprotect)elf_scope_lookup(g_shim_scope, "mprotect");
    int r;
    if (f) {
        r = f(addr, len, eff);
    } else {
        long raw = shim_raw_syscall6(226, (long)addr, (long)len, eff, 0, 0, 0);
        r = (raw < 0 && raw > -4096) ? -1 : 0;
    }
    if (g_vmtrace) {
        char b[160]; char *i = b;
        const char *p = "[MPROT_OV] addr="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)addr, 12);
        p = " len="; while (*p) *i++ = *p++;
        shim_hex(&i, len, 8);
        p = " prot="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)(unsigned)prot, 2);
        p = " eff="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)(unsigned)eff, 2);
        p = " rc="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)(unsigned)r, 2);
        *i++ = '\n';
        shim_raw_syscall6(64, 2, (long)b, (long)(i - b), 0, 0, 0);
    }
    return r;
}
typedef int (*fp_munmap)(void *, unsigned long);
static int shim_munmap(void *addr, unsigned long len) {
    fp_munmap f = (fp_munmap)elf_scope_lookup(g_shim_scope, "munmap");
    int r;
    if (f) {
        r = f(addr, len);
    } else {
        long raw = shim_raw_syscall6(215, (long)addr, (long)len, 0, 0, 0, 0);
        r = (raw < 0 && raw > -4096) ? -1 : 0;
    }
    if (g_vmtrace) {
        char b[128]; char *i = b;
        const char *p = "[MUNMAP] addr="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)addr, 12);
        p = " len="; while (*p) *i++ = *p++;
        shim_hex(&i, len, 8);
        p = " rc="; while (*p) *i++ = *p++;
        shim_hex(&i, (unsigned long)(unsigned)r, 2);
        *i++ = '\n';
        shim_raw_syscall6(64, 2, (long)b, (long)(i - b), 0, 0, 0);
    }
    return r;
}

/* sigprocmask/pthread_sigmask: SIGSYS nikdy neblokovat. SIGSYS nese
 * loaderovy seccomp TRAP (Go mod, Android emulace) a kernel 4.14
 * force_sig_info() pri blokovanem SIGSYS resetuje handler na SIG_DFL ->
 * dalsi TRAP proces zabije. Typicky: cgo x_cgo_thread_start blokuje vse
 * pres pthread_sigmask, nove Go vlakno pak v minit dela raw rt_sigprocmask.
 * Bezi pod guest TP: zadny bionic kod, glibc sigset_t = 128 B. */
static void *g_real_sigprocmask, *g_real_pthread_sigmask;
typedef int (*fp_sigmask)(int, const void *, void *);
static const void *shim_sigset_nosys(const void *set, unsigned long *buf) {
    if (!set) return NULL;
    const unsigned long *s = (const unsigned long *)set;
    for (int i = 0; i < 16; i++) buf[i] = s[i];
    buf[0] &= ~(1UL << 30);                  /* SIGSYS = 31 */
    return buf;
}
static int shim_sigprocmask(int how, const void *set, void *old) {
    unsigned long b[16];
    if (!g_real_sigprocmask) return -1;
    return ((fp_sigmask)g_real_sigprocmask)(how, how == 1 ? set : shim_sigset_nosys(set, b), old);
}
static int shim_pthread_sigmask(int how, const void *set, void *old) {
    unsigned long b[16];
    if (!g_real_pthread_sigmask) return 38;
    return ((fp_sigmask)g_real_pthread_sigmask)(how, how == 1 ? set : shim_sigset_nosys(set, b), old);
}

/* Viz registrace v run_ownall. Bezi pod guest TP: zadny bionic kod. */
static void *g_real_libc_start_main;
typedef int (*fp_libc_start_main)(void *, int, char **, void *, void *, void *, void *);
static int shim_libc_start_main(void *main_fn, int argc, char **argv, void *init,
                                void *fini, void *rtld_fini, void *stack_end) {
    (void)init;   /* konstruktory exe uz spustil loader */
    return ((fp_libc_start_main)g_real_libc_start_main)(main_fn, argc, argv, NULL,
                                                        fini, rtld_fini, stack_end);
}

static f2_hook_t g_f2_hooks[] = {
    {"open",(void*)shim_open,&g_orig_open},{"open64",(void*)shim_open64,&g_orig_open64},
    {"__open",(void*)shim_open,&g_orig_open},{"__open64",(void*)shim_open64,&g_orig_open64},
    {"openat",(void*)shim_openat,&g_orig_openat},{"openat64",(void*)shim_openat64,&g_orig_openat64},
    {"__openat",(void*)shim_openat,&g_orig_openat},{"__openat64",(void*)shim_openat64,&g_orig_openat64},
    {"stat",(void*)shim_stat,&g_orig_stat},{"stat64",(void*)shim_stat64,&g_orig_stat64},
    {"__xstat",(void*)shim___xstat,&g_orig___xstat},{"lstat",(void*)shim_lstat,&g_orig_lstat},
    {"__lxstat",(void*)shim___lxstat,&g_orig___lxstat},
    {"lstat64",(void*)shim_lstat64,&g_orig_lstat64},
    {"fstat64",(void*)shim_fstat64,&g_orig_fstat64},
    {"fstatat64",(void*)shim_fstatat64,&g_orig_fstatat64},
    {"statvfs",(void*)shim_statvfs,&g_orig_statvfs},
    {"statvfs64",(void*)shim_statvfs64,&g_orig_statvfs64},
    {"access",(void*)shim_access,&g_orig_access},{"euidaccess",(void*)shim_euidaccess,&g_orig_euidaccess},{"faccessat",(void*)shim_faccessat,&g_orig_faccessat},
    {"statx",(void*)shim_statx,&g_orig_statx},{"fstatat",(void*)shim_fstatat,&g_orig_fstatat},
    {"newfstatat",(void*)shim_newfstatat,&g_orig_newfstatat},{"__fxstatat",(void*)shim___fxstatat,&g_orig___fxstatat},
    {"symlink",(void*)shim_symlink,&g_orig_symlink},{"symlinkat",(void*)shim_symlinkat,&g_orig_symlinkat},
    {"link",(void*)shim_link,&g_orig_link},{"rename",(void*)shim_rename,&g_orig_rename},
    {"renameat",(void*)shim_renameat,&g_orig_renameat},{"renameat2",(void*)shim_renameat2,&g_orig_renameat2},
    {"unlink",(void*)shim_unlink,&g_orig_unlink},{"unlinkat",(void*)shim_unlinkat,&g_orig_unlinkat},
    {"rmdirat",(void*)shim_rmdirat,&g_orig_rmdirat},{"mkdir",(void*)shim_mkdir,&g_orig_mkdir},
    {"mkdirat",(void*)shim_mkdirat,&g_orig_mkdirat},{"rmdir",(void*)shim_rmdir,&g_orig_rmdir},
    {"execve",(void*)shim_execve,&g_orig_execve},{"execv",(void*)shim_execv,&g_orig_execv},
    {"execvp",(void*)shim_execvp,&g_orig_execvp},{"execvpe",(void*)shim_execvpe,&g_orig_execvpe},
    {"execveat",(void*)shim_execveat,&g_orig_execveat},
    {"execl",(void*)shim_execl,&g_orig_execl},{"execlp",(void*)shim_execlp,&g_orig_execlp},
    {"execle",(void*)shim_execle,&g_orig_execle},
    {"posix_spawnp",(void*)shim_posix_spawnp,&g_orig_posix_spawnp},
    {"posix_spawn",(void*)shim_posix_spawnp,&g_orig_posix_spawn},
    {"opendir",(void*)shim_opendir,&g_orig_opendir},
    {"readlink",(void*)shim_readlink,&g_orig_readlink},{"readlinkat",(void*)shim_readlinkat,&g_orig_readlinkat},
    {"realpath",(void*)shim_realpath,&g_orig_realpath},{"dlopen",(void*)shim_dlopen,&g_orig_dlopen},
    {"chdir",(void*)shim_chdir,&g_orig_chdir},
    {"fopen",(void*)shim_fopen,&g_orig_fopen},{"fopen64",(void*)shim_fopen64,&g_orig_fopen64},
    {"__xstat64",(void*)shim___xstat64,&g_orig___xstat64},{"__lxstat64",(void*)shim___lxstat64,&g_orig___lxstat64},
    {"__fxstatat64",(void*)shim___fxstatat64,&g_orig___fxstatat64},{"faccessat2",(void*)shim_faccessat2,&g_orig_faccessat2},
    {"getrlimit",(void*)shim_getrlimit,&g_orig_getrlimit},{"__getrlimit",(void*)shim_getrlimit,&g_orig_getrlimit},
    {"fileno_unlocked",(void*)shim_fileno_unlocked,&g_orig_fileno_unlocked},{"fileno",(void*)shim_fileno,&g_orig_fileno},
    {"setfsuid",(void*)shim_setfsuid,&g_orig_setfsuid},
    {"setfsgid",(void*)shim_setfsgid,&g_orig_setfsgid},
    /* close/flockfile/mprotect jsou diagnosticke shimy (Node ladeni) a
     * prlimit64 mel spatnou signaturu - dokud MAX_OVERRIDES=64 zahazoval vse
     * od 65. polozky, nikdy nebezely. Registrovat jen explicitne. */
};

static int f2_only_match(const char *name) {
    const char *only = getenv("F2_ONLY");
    if (!only || !only[0]) return 1;
    const char *p = only;
    while (*p) {
        const char *c = p;
        while (*c && *c != ',') c++;
        size_t l = (size_t)(c - p);
        if (l == strlen(name) && strncmp(p, name, l) == 0) return 1;
        p = (*c) ? c + 1 : c;
    }
    return 0;
}

/* F2 setup: (1) zaregistrujeme symbol-override pro PLT binarky (binary vidi
 * shim primo pres vlastni PLT), (2) patchneme glibc leaf funkce inline-hookem,
 * aby se zachytily i glibc-interni volani (opendir->openat64 raw syscall). */
/* Raw syscall (aarch64) - TP-independent. Shim funkce jsou volany z guest
 * libc (aktivni parrot TP), kde bionicky syscall() potrebuje bionicky TP kvuli
 * errno -> pouzijeme primy svc #0. */
static long main_raw_syscall4(long nr, long a0, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory", "cc");
    return x0;
}

/* Interni glibc volani __open64_nocancel (GLIBC_PRIVATE) obchazi PLT override
 * i inline hooky na open/open64 - proto guest glibc cetl host /etc/resolv.conf
 * (neexistuje) a DNS nefungovalo. Tato funkce je volana i s cestami jako
 * /etc/nsswitch.conf, /etc/hosts apod. Prelozime cestu pod ROOTFS.
 * Diky tomu NEMUSIME mit seccomp F2 filtr (ktery otravoval fork+exec deti). */
static int shim_open64_nocancel(const char *p, int flags, ...) {
    /* Preloz cestu a zavolej ORIGINAL pres trampolinu (g_orig_open64_nocancel).
     * Original nastavi errno spravne (glibc __open64_nocancel semantika) - proto
     * to nerobime raw syscallem, ktery errno neaktualizuje a rozbil
     * dl_iterate_phdr/NSS v id/whoami/zsh. */
    /* CILENY preklad: __open64_nocancel je volan z glibc internich cest
     * (dlopen NSS modulu, _dl_map_object). Preklad VSECH cest rozbil
     * dl_iterate_phdr (NULL linkmap) -> pad id/whoami/zsh. Proto prekladame
     * JEN DNS soubory, ktere NSS/DNS resolver cte internim nocancel volanim
     * (a ktere PLT override nechyti). Ostatni cesty -> passthrough. */
    /* Prekladame JEN konkretni /etc/ konfiguraky, ktere glibc cte internim
     * nocancel volanim (obchazi PLT override): DNS resolver (resolv.conf),
     * NSS files backend (passwd/group/nsswitch/hosts).
     * Ostatni /etc/ (ld.so.cache, ld.so.preload) a /usr/lib (dlopen NSS
     * modulu) NEprekladame - jejich preklad rozbil knihovny/linkmapy
     * (DNS prestal fungovat, dl_iterate_phdr NULL deref). */
    static const char *etc_files[] = {
        "/etc/resolv.conf", "/etc/passwd", "/etc/group",
        "/etc/nsswitch.conf", "/etc/hosts",
        /* /etc/protocols + /etc/services: NSS files backend je cte internim
         * nocancel volanim pri getprotobyname/getservbyname (ping/ping6).
         * Bez prekladu cte host /etc/protocols (neexistuje) -> ENOENT ->
         * "ping: unknown protocol icmp". Overeno: nativne ping RC=0,
         * pod loaderem RC=1; strace ukazal openat("/etc/protocols")=ENOENT. */
        "/etc/protocols", "/etc/services", NULL
    };
    const char *path = p;
    char buf[8192];
    if (p && p[0] == '/') {
        for (int i = 0; etc_files[i]; i++) {
            size_t fl = shim_strlen(etc_files[i]);
            if (shim_strncmp(p, etc_files[i], fl) == 0 &&
                (p[fl] == 0 || p[fl] == '/')) {
                if (shim_translate(p, buf, sizeof buf)) path = buf;
                break;
            }
        }
    }
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    fp_open f = (fp_open)g_orig_open64_nocancel;
    return f ? f(path, flags, mode) : -1;
}

/* Inline hook pro funkce s PAC prologem (paciasp) - hook_install je odmita,
 * protoze prvni instrukce neni B-thunk. Zkopirujeme prvni instrukci do
 * trampoliny, za ni B na target+4, a target prejdeme na B->shim. PAC zustava
 * konzistentni (trampolina dela paciasp/autiasp se stejnym SP). */
static int hook_inline_prologue(void *target, void *shim, void **orig) {
    if (!target || !shim) return 0;
    uint32_t ins0 = *(const uint32_t *)target;
    /* Odmitni B/BL/ADRP/LDR-lit prolog (thunk pattern) - PAC (d503233f) je OK. */
    uint32_t cls = ins0 & 0xFC000000;
    if (cls == 0x14000000 || cls == 0x94000000) return 0;
    if ((ins0 & 0x9F000000) == 0x90000000) return 0;
    if ((ins0 & 0xBF000000) == 0x18000000) return 0;
    /* Trampolina: zkopiruj prvni instrukci, za ni B na target+4. Shim pak
     * muze zavolat g_orig (= trampolina) a zachovat glibc errno semantiku.
     * (Raw syscall v shimu obchazel errno -> rozbity dl_iterate_phdr/NSS.) */
    void *tramp = alloc_near(target);
    if (tramp == MAP_FAILED) return 0;
    *(uint32_t *)tramp = ins0;
    uint32_t bi = branch_insn((char *)tramp + 4, (char *)target + 4);
    if (!bi) {
        void *b = make_bridge((char *)target + 4);
        if (!b) return 0;
        bi = branch_insn((char *)tramp + 4, b);
        if (!bi) return 0;
    }
    *(uint32_t *)((char *)tramp + 4) = bi;
    __builtin___clear_cache(tramp, (char *)tramp + 8);
    mprotect(tramp, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (patch_branch(target, shim) != 0) return 0;
    *orig = tramp;
    return 1;
}

static void shim_register_overrides(void) {
    if (getenv("F2_DISABLE")) return;  /* debug: zadne override */
    g_shim_root = getenv("ROOTFS");
    g_shim_loader = getenv("ELF_LOADER");
    if (!g_shim_loader || !g_shim_loader[0]) g_shim_loader = "/proc/self/exe";
    wl_init();   /* nacti whitelist + priprav white.log (bionicky kontext) */
    for (size_t i = 0; i < sizeof g_f2_hooks / sizeof g_f2_hooks[0]; i++)
        if (f2_only_match(g_f2_hooks[i].n))
            elf_register_override(g_f2_hooks[i].n, g_f2_hooks[i].shim);
}
static void shim_install_hooks(void) {
    if (getenv("F2_DISABLE")) return;
    int ok = 0;
    for (size_t i = 0; i < sizeof g_f2_hooks / sizeof g_f2_hooks[0]; i++)
        if (f2_only_match(g_f2_hooks[i].n) && hook_install(&g_f2_hooks[i])) ok++;
    /* Interni glibc open (__open64_nocancel) obchazi PLT override i inline
     * hooky na open/open64 - proto guest glibc cetl host /etc/resolv.conf
     * (neexistuje) a DNS nefungovalo. Inline-hook (PAC prolog) ho prelozi pod
     * ROOTFS. Nahrazuje seccomp F2 filtr, ktery zabijel fork+exec deti
     * (subprocess/zsh -i). Vypnout lze F2_NO_INTERNAL_HOOK=1.
     * POZOR: musi bezet az po elf_load (scope ma moduly), proto zde a ne
     * v shim_register_overrides. */
    if (g_shim_scope && !getenv("F2_NO_INTERNAL_HOOK")) {
        void *t = elf_scope_lookup(g_shim_scope, "__open64_nocancel");
        if (!t) t = elf_scope_lookup(g_shim_scope, "__open_nocancel");
        if (elf_debug())
            fprintf(stderr, "[hook] __open64_nocancel lookup=%p\n", t);
        if (t && hook_inline_prologue(t, (void *)shim_open64_nocancel,
                                      &g_orig_open64_nocancel)) {
            ok++;
            if (elf_debug())
                fprintf(stderr, "[hook] __open64_nocancel inline OK\n");
        } else if (t && elf_debug()) {
            fprintf(stderr, "[hook] __open64_nocancel inline FAIL\n");
        }
    }
    /* glibc 2.41 ma na zacatku open64/openat64 PAC `paciasp` -> hook_install
     * (jen B-thunky) je preskoci a glibc-interni volani (fopen -> __open64)
     * cetla host cesty: git "fatal: error processing config file(s)" pri
     * fopen("/etc/gitconfig"). Stejny preklad jako PLT override (shim_open64)
     * pres PAC-safe inline hook. open/__open/__open64 sdileji adresu s open64.
     * Vypnout lze F2_NO_OPEN_HOOK=1. */
    if (g_shim_scope && !getenv("F2_NO_OPEN_HOOK")) {
        static const struct { const char *n; void *shim; void **orig; void **alias; } oh[] = {
            { "open64",   (void *)shim_open64,   &g_orig_open64,   &g_orig_open },
            { "openat64", (void *)shim_openat64, &g_orig_openat64, &g_orig_openat },
        };
        for (size_t i = 0; i < sizeof oh / sizeof oh[0]; i++) {
            if (*oh[i].orig) continue;             /* uz hooknuto (B-thunk) */
            void *t = elf_scope_lookup(g_shim_scope, oh[i].n);
            void *tramp = NULL;
            if (t && hook_inline_prologue(t, oh[i].shim, &tramp) && tramp) {
                *oh[i].orig = tramp;
                if (!*oh[i].alias) *oh[i].alias = tramp;
                ok++;
                if (elf_debug())
                    fprintf(stderr, "[hook] %s inline (PAC) OK\n", oh[i].n);
            }
        }
    }
    if (elf_debug())
        fprintf(stderr, "[F2-hooks] inline-patchnuto %d glibc funkci\n", ok);
}

/* Fallback: kdyz inline-hook (patch glibc) selhal (W^X na zarizeni blokuje
 * mprotect RWX na kodu glibc), naplnime g_orig_* realnou funkci z glibc scope,
 * aby shim funkce mohly zavolat skutecny open/stat/... (bez toho by g_orig_*
 * zustalo NULL a F2 by pro open/fopen programy vubec nefungovalo). Na
 * non-W^X zarizeni (hook uspel) uz je g_orig_* = trampolina, takze preskocime. */
static void shim_resolve_fallback(void) {
    if (getenv("F2_DISABLE")) return;
    for (size_t i = 0; i < sizeof g_f2_hooks / sizeof g_f2_hooks[0]; i++) {
        if (!f2_only_match(g_f2_hooks[i].n)) continue;
        if (*g_f2_hooks[i].orig != NULL) continue;   /* uz nastavil inline-hook */
        void *real = elf_scope_lookup(g_shim_scope, g_f2_hooks[i].n);
        if (real) *g_f2_hooks[i].orig = real;
    }
}

/* F2 (path-translation shim) = own-loading (parrot glibc) + GOT/PLT override.
 * Override "open"/"openat"/"stat"/... prepisuje absolutni cesty "/" -> "$ROOTFS/".
 * Protoze parrot glibc neni -Bsymbolic, zachyti i glibc-interni volani
 * (fopen->open). Loader zustava v procesu, takze jeho SIGSYS handler funguje
 * (vyhneme se parrot ld.so, ktery na tomto zarizeni narazi na app-seccomp
 * RET_KILL a zabije proces). Zadny ptrace, zadna syscall translace. */
static int run_shim(const char *path, int argc, char **argv, char **envp) {
    g_f2_active = 1;
    g_exec_mode = "--shim";
    if (elf_debug())
        fprintf(stderr, "[+] F2 path-translation shim (ROOTFS=%s)\n",
                g_shim_root ? g_shim_root : "(unset)");
    return run_ownall(path, argc, argv, envp);
}

static int run_own(const char *path, const char *mod, int argc, char **argv,
                   char **envp) {
    elf_init_argc = argc;
    elf_init_argv = argv;
    elf_init_envp = envp;
    elf_object_t *obj = elf_load(path);
    if (!obj) {
        fprintf(stderr, "[-] Failed to load ELF\n");
        return 1;
    }

    obj->scope = elf_scope_create();
    if (!obj->scope) {
        elf_unload(obj);
        return 1;
    }
    if (mod)
        elf_load_shared(mod, obj->scope);
    if (elf_debug())
        printf("[+] scope: %zu modules\n", obj->scope->count);

    if (elf_relocate(obj) != 0) {
        fprintf(stderr, "[-] Relocation failed\n");
        elf_scope_destroy(obj->scope);
        obj->scope = NULL;
        elf_unload(obj);
        return 1;
    }

    int ret = elf_run(obj, argc, argv, envp);
    elf_scope_destroy(obj->scope);
    obj->scope = NULL;
    elf_unload(obj);
    return ret;
}

static int run_ownall(const char *path, int argc, char **argv, char **envp) {
    elf_install_fault_handlers();
    /* Zajisti fd 0/1/2. V Android app sandboxu (ashell -c) byva fd 0 zavreny;
     * prvni loaderuv open() pak dostane fd 0 a jeho close() zavre "stdin".
     * Node/libuv na to pada: uv__close Assertion `fd > STDERR_FILENO`.
     * Python3 -c padal na fileno(stdin=NULL). Otevreme /dev/null pro chybejici
     * standardni fd (fcntl F_GETFD vrati -1/EBADF). */
    for (int _fd = 0; _fd <= 2; _fd++) {
        if (fcntl(_fd, F_GETFD) == -1) {
            int _n = open("/dev/null", O_RDWR);
            if (_n < 0) { _n = open("/dev/null", O_RDONLY); }
            if (_n >= 0 && _n != _fd) { dup2(_n, _fd); close(_n); }
        }
    }
    g_tls_trace = getenv("ELF_LOADER_TLS_TRACE") != NULL;
    g_vmtrace = getenv("ELF_LOADER_VMTRACE") != NULL;
    g_ro_keep_write = getenv("ELF_LOADER_RO_KEEP_WRITE") != NULL;
    elf_scope_t *scope = elf_scope_create();
    if (!scope) {
        fprintf(stderr, "[-] scope alloc failed\n");
        return 1;
    }
    elf_own_deps = 1;
    elf_own_scope = scope;
    g_shim_scope = scope;
    elf_init_argc = argc;
    elf_init_argv = argv;
    elf_init_envp = envp;

    /* Guest ld.so (parrot glibc) je zaveden jako běžná .so, ale jeho vlastní
     * _dl_start neproběhl — interní _rtld_global (base+0x40000) a malloc cache
     * (base+0x3fb00) jsou nuly. Když libc přes lazy JUMP_SLOT zavolá
     * _dl_allocate_tls, resolve_jmp_symbol nejdřív zkusí scope lookup a najde
     * guest ld.so _dl_allocate_tls@base+0xfeb0 → ta spadne na NULL.
     * override_lookup je v resolve_jmp_symbol PRVNÍ, takže registrace těchto
     * override zajistí, že libc (pthread_create) dostane naši funkční verzi. */
    elf_register_override("_dl_allocate_tls", (void *)ldso_allocate_tls);
    elf_register_override("_dl_allocate_tls_init", (void *)ldso_allocate_tls_init);
    elf_register_override("_dl_deallocate_tls", (void *)ldso_deallocate_tls);
    /* Guest glibc dlopen/dlsym/dlerror/dlclose/dladdr pracuji nad _rtld_global,
     * ktery pri nasem own-loadu zustava nulovy -> kazde volani (Rust uv hleda
     * gnu_get_libc_version, Python _ctypes volá dlsym) spadne. Registrujeme
     * nase nahrady jako OVERRIDE (jdou v overide_lookup prvni, v lazy i eager
     * ceste), aby se nikdy neresolvovaly na rozbite guest glibc symboly. */
    elf_register_override("dlopen", (void *)ldso_dlopen);
    elf_register_override("dlopen64", (void *)ldso_dlopen);
    elf_register_override("__dlopen", (void *)ldso_dlopen);
    elf_register_override("dlsym", (void *)ldso_dlsym);
    elf_register_override("__dlsym", (void *)ldso_dlsym);
    elf_register_override("dlclose", (void *)ldso_dlclose);
    elf_register_override("dlerror", (void *)ldso_dlerror);
    elf_register_override("dladdr", (void *)ldso_dladdr);
    if (getenv("ELF_LOADER_ASSERT_TRACE"))
        elf_register_override("__assert_fail", (void *)shim_assert_fail);
    /* pthread_create fix: vzdy - glibc EINVAL kvuli velkemu TLS static size. */
    elf_register_override("pthread_create", (void *)shim_pthread_create);
    elf_register_override("sigprocmask", (void *)shim_sigprocmask);
    elf_register_override("canonicalize_file_name", (void *)shim_canonicalize_file_name);
    elf_register_override("__realpath_chk", (void *)shim_realpath_chk);
    elf_register_override("pthread_sigmask", (void *)shim_pthread_sigmask);
    /* Stara ABI (__libc_start_main@GLIBC_2.17, binarky linkovane proti glibc
     * < 2.34: node, Bun...) predava z _start init=__libc_csu_init a nova glibc
     * ho zavola -> konstruktory hlavni binarky bezely PODRUHE (loader je uz
     * spustil v elf_run_final). Dvakrat registrovany static destruktor pak
     * pri exitu delal double free (node: ~SnapshotData -> free(): invalid
     * pointer, EXIT=134). Shim init vynuluje; nova ABI predava NULL stejne. */
    elf_register_override("__libc_start_main", (void *)shim_libc_start_main);
    elf_register_override("pthread_getattr_np", (void *)shim_pthread_getattr_np);
    /* mmap/mmap64: emulace MAP_FIXED_NOREPLACE (kernel 4.14 ho nezna).
     * V8/Node s nim rezervuje 4GB pointer-compression cage na presne adrese;
     * bez emulace dostane jinou adresu -> divoke komprimovane pointery. */
    elf_register_override("mmap", (void *)shim_mmap);
    elf_register_override("mmap64", (void *)shim_mmap64);
    if (g_vmtrace) elf_register_override("munmap", (void *)shim_munmap);
    if (g_vmtrace || g_ro_keep_write)
        elf_register_override("mprotect", (void *)shim_mprotect_ov);
    /* sigaction override: zachyti instalaci fatal-signal handleru a ulozi
     * ho do g_guest_fatal. Lze vypnout ELF_LOADER_NO_SA_OVERRIDE=1 (pak si
     * V8/node instaluje sve handlery a nas fault dump se nezobrazi). */
    if (!getenv("ELF_LOADER_NO_SA_OVERRIDE"))
        elf_register_override("sigaction", (void *)diag_wrapped_sigaction);


    if (!g_exec_mode) g_exec_mode = "--ownall";
    g_shim_root = getenv("ROOTFS");
    g_shim_loader = getenv("ELF_LOADER");
    /* ROOTFS neni v env (ad-hoc spusteni: loader --ownall /abs/guest/bin/x):
     * odvodime distro root z cesty spustene binarky. Bez toho zustane
     * g_shim_root NULL, search_guest_path nehleda v rootfs a fork+exec deti
     * (tar -> gzip) dostanou loader bez rootfs -> "dep libc.so not found". */
    if (!g_shim_root || !g_shim_root[0]) {
        const char *derived = shim_derive_root_from_path(path);
        if (derived && derived[0]) {
            g_shim_root = derived;
            if (elf_debug())
                fprintf(stderr, "[+] derived ROOTFS from exe path: %s\n", derived);
        }
    }
    if (g_shim_root && g_shim_root[0]) {
        g_f2_active = 1;
        setenv("ROOTFS", g_shim_root, 1);
        /* Interaktivni shell/tmux compat defaulty (viz postup.md pokracovani 17):
         * bez LOCPATH/LC_ALL hlasi tmux "need UTF-8 locale" (glibc hleda
         * /usr/lib/locale na hostu, ne v rootfs); bez SHELL padne na
         * /usr/sbin/nologin. setenv(...,0) - nepreepise, co uz nastavil
         * uzivatel/prostredi. access() check, aby se nenastavilo na
         * neexistujici cestu v rootfs, ktery tohle nema. */
        char locpath[1024], shellpath[1024];
        int n;
        n = snprintf(locpath, sizeof locpath, "%s/usr/lib/locale", g_shim_root);
        if (n > 0 && (size_t)n < sizeof locpath && access(locpath, F_OK) == 0)
            setenv("LOCPATH", locpath, 0);
        /* C.UTF-8 je vestaveny glibc locale (od 2.35, bez potreby locale-archive
         * dat) - nastavit vzdy, nezavisi na existenci LOCPATH adresare. */
        setenv("LC_ALL", "C.UTF-8", 0);
        n = snprintf(shellpath, sizeof shellpath, "%s/bin/bash", g_shim_root);
        if (n > 0 && (size_t)n < sizeof shellpath && access(shellpath, X_OK) == 0)
            setenv("SHELL", shellpath, 0);
    }
    if (!g_shim_loader || !g_shim_loader[0]) {
        static char self_exe[1024];
        ssize_t n = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
        if (n > 0) {
            self_exe[n] = '\0';
            g_shim_loader = self_exe;
        } else {
            g_shim_loader = "/proc/self/exe";
        }
    }
    if (g_shim_loader && g_shim_loader[0]) setenv("ELF_LOADER", g_shim_loader, 1);

    /* Shebang podpora na urovni loader entry: `lx modal` predava
     * /usr/local/bin/modal (Python skript s #!/usr/bin/python3), elf_load pak
     * hlasi "Not an ELF file". Detekujeme #! v prvnim radku a prepiseme
     * path/argv na [interp, (interp_arg,) script, orig_args...]. Interpret
     * pod guest ROOTFS (napr. /usr/bin/python3 -> $ROOTFS/usr/bin/python3). */
    {
        /* Entry symlink resolv PRED shebang detekci: `which` je
         * $R/usr/bin/which -> /etc/alternatives/which -> /usr/bin/which.debianutils
         * (skript). Cile symlinku jsou v rootfs absolutni host cesty, takze
         * prime open(path) na hostu selze (broken symlink) -> shebang se
         * nedetekuje a elf_load pak hlasi "Not an ELF file". Resolvneme chain
         * pod ROOTFS (shim_resolve_symlinks), aby se #! naslo. */
        static char _resolved[8192];   /* static: path na nej muze ukazovat i po bloku */
        if (shim_resolve_symlinks(path, _resolved, sizeof(_resolved)))
            path = _resolved;
        int _fd = open(path, O_RDONLY | O_CLOEXEC);
        if (_fd >= 0) {
            char hdr[256];
            ssize_t n = read(_fd, hdr, sizeof(hdr) - 1);
            close(_fd);
            if (n >= 2 && hdr[0] == '#' && hdr[1] == '!') {
                hdr[n] = 0;
                char *line = hdr + 2;
                while (*line == ' ' || *line == '\t') line++;
                char *eol = strchr(line, '\n');
                if (eol) *eol = 0;
                char *cr = strchr(line, '\r');
                if (cr) *cr = 0;

                char *arg1 = line;
                while (*arg1 && *arg1 != ' ' && *arg1 != '\t') arg1++;
                char *interp_arg = NULL;
                if (*arg1) {
                    *arg1 = 0; arg1++;
                    while (*arg1 == ' ' || *arg1 == '\t') arg1++;
                    if (*arg1) interp_arg = arg1;
                }

                static char interp_buf[4096];
                interp_buf[0] = 0;
                size_t rl = g_shim_root ? strlen(g_shim_root) : 0;
                if (strcmp(line, "/usr/bin/env") == 0 && interp_arg) {
                    char *sp = strchr(interp_arg, ' ');
                    if (sp) *sp = 0;
                    if (rl)
                        snprintf(interp_buf, sizeof(interp_buf), "%s/usr/bin/%s", g_shim_root, interp_arg);
                    else
                        snprintf(interp_buf, sizeof(interp_buf), "%s", interp_arg);
                    interp_arg = NULL;
                } else if (rl && line[0] == '/' &&
                           strncmp(line, g_shim_root, rl) != 0) {
                    snprintf(interp_buf, sizeof(interp_buf), "%s%s", g_shim_root, line);
                } else {
                    snprintf(interp_buf, sizeof(interp_buf), "%s", line);
                }

                int extra = interp_arg ? 2 : 1;
                int new_argc = argc + extra;
                static char *new_argv[512];
                if (new_argc + 1 <= (int)(sizeof(new_argv) / sizeof(new_argv[0]))) {
                    int k = 0;
                    new_argv[k++] = interp_buf;
                    if (interp_arg) new_argv[k++] = interp_arg;
                    new_argv[k++] = (char *)path;
                    for (int i = 1; i < argc; i++) new_argv[k++] = argv[i];
                    new_argv[k] = NULL;
                    argc = new_argc;
                    argv = new_argv;
                    path = interp_buf;
                    elf_init_argc = argc;
                    elf_init_argv = argv;
                    if (elf_debug())
                        fprintf(stderr, "[+] shebang: interp=%s\n", interp_buf);
                }
            }
        }
    }

    shim_register_overrides();

    g_loader_active = 1;  /* loaderuv kod (bionic TLS): jeho open = bionicky */
    /* F2 seccomp filtr je volitelny (F2_FILTER=1). Pri re-execu (shim_execve)
     * dedi dite filtr, ale SIGSYS handler je po execve SIG_DFL a bionic ld.so
     * ditete dela openat jeste pred main() -> SIGSYS -> pad. Proto je filtr
     * defaultne VYPNUTY; preklad cest zajistuji PLT override + inline hooky +
     * explicitni reseni symlinku v elf_load (resolve_symlinks_under_root). */
    if (g_f2_active) {
        f2_set_root(g_shim_root);
        /* --ownall potrebuje path translation i pro glibc-interni volani
         * (__open64_nocancel -> /etc/resolv.conf pro DNS), ktera obchazeji
         * PLT override. Proto filtr zapiname DEFAULTNE (lze vypnout
         * F2_FILTER=0). */
        if (f2_should_filter()) {
            install_f2_path_filter();
        }
    }
    elf_object_t *obj = elf_load(path);
    elf_own_scope = NULL;
    elf_set_crash_scope(scope);
    if (!obj) {
        elf_scope_destroy(scope);
        return 1;
    }
    /* Go binarky volaji syscally napric (mimo PLT overridy) -> seccomp
     * path-preklad omezeny na jejich text segment. ELF_LOADER_GOMODE=0 vypne. */
    if (g_shim_root && g_shim_root[0]) {
        const char *gm = getenv("ELF_LOADER_GOMODE");
        if (!gm || gm[0] != '0') {
            f2_set_root(g_shim_root);
            f2_set_loader(g_shim_loader);
            elf_go_mode_setup(obj);
        }
    }
    shim_install_hooks();    /* patch glibc leaf funkci (F2 / re-exec) */
    shim_resolve_fallback(); /* fallback real funkci (W^X) */
    g_real_libc_start_main = elf_scope_lookup(scope, "__libc_start_main");
    g_real_sigprocmask = elf_scope_lookup(scope, "sigprocmask");
    g_real_canonicalize = elf_scope_lookup(scope, "canonicalize_file_name");
    g_real_realpath_chk = elf_scope_lookup(scope, "__realpath_chk");
    g_real_pthread_sigmask = elf_scope_lookup(scope, "pthread_sigmask");

    /* FAKEROOT mod (volitelne): kdyz ELF_LOADER_FAKEROOT urcuje cestu k
     * libfakeroot-tcp.so, own-loadneme ji jako PRVNI modul ve scope (pred
     * libc), aby fakeroot chown/stat/getuid wrappery vyhraly v elf_scope_find
     * pro PLT volani z guest binarky (dpkg/apt). Fakeroot si realne libc
     * funkce najde sam pres dlopen("libc.so.6")+dlsym(libc_handle, name) -
     * nas ldso_dlopen ho prelozi a vrati handle na skutecny libc objekt.
     * Nase override (open/openat, ...) ma vzdy prioritu, takze path-translation
     * zustane nase. Vypnuti: ELF_LOADER_NO_FAKEROOT=1. */
    const char *fr_path = getenv("ELF_LOADER_FAKEROOT");
    if (fr_path && fr_path[0] && !getenv("ELF_LOADER_NO_FAKEROOT")) {
        elf_object_t *fm = elf_load_shared(fr_path, scope);
        if (fm) {
            /* posun na index 0 (pred libc, ktery elf_load_shared pridal prvni) */
            if (scope->count > 1 && scope->mods[0] != fm) {
                size_t fi = 0;
                for (size_t k = 0; k < scope->count; k++)
                    if (scope->mods[k] == fm) { fi = k; break; }
                for (size_t k = fi; k > 0; k--)
                    scope->mods[k] = scope->mods[k-1];
                scope->mods[0] = fm;
            }
            if (elf_debug())
                fprintf(stderr, "[fakeroot] load OK: %s (idx 0, count=%zu)\n",
                        fr_path, scope->count);
        } else if (elf_debug()) {
            fprintf(stderr, "[fakeroot] load FAILED: %s\n", fr_path);
        }
    }

    /* GUEST HELPER knihovny (volitelne): ELF_LOADER_HELPER=/a.so[:/b.so...]
     * Vlastni .so zkompilovane proti glibc (v Parrotu obycejnym gcc), ktere
     * own-loadneme PRED vsechny ostatni moduly (i pred fakeroot a libc).
     * Jejich symboly tak vyhraji v elf_scope_find pro PLT volani guestu.
     * Vyhoda proti shimum v loaderu: kod bezi v glibc svete pod guest TP,
     * smi normalne volat printf/malloc/getenv bez prepinani TPIDR_EL0.
     * Realnou funkci najde pres dlsym(RTLD_NEXT, ...).
     * Omezeni: (1) symboly z override tabulky loaderu (open/stat/exec...)
     * maji porad prednost; (2) volani uvnitr glibc (mimo PLT) nechyti.
     * Promenna se dedi do re-exec deti, takze helper plati i pro ne. */
    {
        const char *hp = getenv("ELF_LOADER_HELPER");
        if (hp && hp[0]) {
            char hbuf[4096];
            strncpy(hbuf, hp, sizeof hbuf - 1);
            hbuf[sizeof hbuf - 1] = '\0';
            size_t ins = 0;
            char *save = NULL;
            for (char *tok = strtok_r(hbuf, ":", &save); tok;
                 tok = strtok_r(NULL, ":", &save)) {
                if (!tok[0]) continue;
                elf_object_t *hm = elf_load_shared(tok, scope);
                if (!hm) {
                    fprintf(stderr, "[helper] load FAILED: %s\n", tok);
                    continue;
                }
                size_t fi = ins;
                for (size_t k = 0; k < scope->count; k++)
                    if (scope->mods[k] == hm) { fi = k; break; }
                if (fi > ins) {
                    for (size_t k = fi; k > ins; k--)
                        scope->mods[k] = scope->mods[k-1];
                    scope->mods[ins] = hm;
                }
                if (elf_debug())
                    fprintf(stderr, "[helper] load OK: %s (idx %zu, count=%zu)\n",
                            tok, ins, scope->count);
                ins++;
            }
        }
    }

    /* GUEST PRELOAD (analogicke k LD_PRELOAD, ale pro --ownall guest scope):
     * ELF_LOADER_PRELOAD=/a.so[:/b.so...]. Guest ld.so pod --ownall nebezi,
     * takze LD_PRELOAD v envp nikdo nezpracuje - loader tedy musi tyto .so
     * pripojit do guest scope sam. Semanticky identicke s HELPER: own-load
     * pred vsechny ostatni moduly (index 0), symboly vyhrajou v PLT lookupu
     * guestu. Ctor sam volan pres queue_module_inits nize.
     * Rozdil od HELPER: PRELOAD jsou "systemove" pomocnici (napr. exec_shim
     * pro re-exec glibc binarek pres loader), typicky nastavene ze zshrc/lx.
     * Vlozeni jde ZA HELPER, takze PRELOAD zaznamy skonci pred nimi. */
    {
        const char *pp = getenv("ELF_LOADER_PRELOAD");
        if (pp && pp[0]) {
            char pbuf[4096];
            strncpy(pbuf, pp, sizeof pbuf - 1);
            pbuf[sizeof pbuf - 1] = '\0';
            size_t ins = 0;
            char *save = NULL;
            for (char *tok = strtok_r(pbuf, ":", &save); tok;
                 tok = strtok_r(NULL, ":", &save)) {
                if (!tok[0]) continue;
                elf_object_t *pm = elf_load_shared(tok, scope);
                if (!pm) {
                    fprintf(stderr, "[preload] load FAILED: %s\n", tok);
                    continue;
                }
                size_t fi = ins;
                for (size_t k = 0; k < scope->count; k++)
                    if (scope->mods[k] == pm) { fi = k; break; }
                if (fi > ins) {
                    for (size_t k = fi; k > ins; k--)
                        scope->mods[k] = scope->mods[k-1];
                    scope->mods[ins] = pm;
                }
                if (elf_debug())
                    fprintf(stderr, "[preload] load OK: %s (idx %zu, count=%zu)\n",
                            tok, ins, scope->count);
                ins++;
            }
        }
    }

    void *libc_obj = NULL;
    for (size_t mi = 0; mi < scope->count; mi++)
        if (scope->mods[mi]->soname && strstr(scope->mods[mi]->soname, "libc.so.6"))
            libc_obj = scope->mods[mi]->base_addr;
    g_libc_base = (uintptr_t)libc_obj;

    void *stacksize_sym = elf_scope_lookup(scope, "__default_stacksize");
    if (stacksize_sym) {
        *(size_t *)stacksize_sym = 8 * 1024 * 1024;
    }

    if (elf_relocate(obj) != 0) {
        fprintf(stderr, "[-] Relocation failed\n");
        elf_scope_destroy(scope);
        obj->scope = NULL;
        elf_unload(obj);
        return 1;
    }

    /* Hlavni exe: elf_load() inity nequeueuje (relokuje se az tady), takze
     * konstruktory hlavniho programu musime zaradit rucne. Bez toho nebezi
     * napr. OpenSSL ctor v node (staticky linkovany) -> zadny provider ->
     * CHECK(ncrypto::CSPRNG(nullptr,0)) assert. */
    elf_queue_module_inits(obj);

    /* ELF_LOADER_TRACE_CALL=0xADDR[,0xADDR...]: nainstaluj call-site tracer
     * (viz install_call_trace) na kazdou zadanou adresu. Diagnostika node
     * SIGSEGV v Builtins_InterpreterEntryTrampoline - overuje, zda jsou
     * argumenty do V8 JSEntry (x0-x5/x8) uz spatne PRED volanim generovaneho
     * kodu, nebo se korupce deje az uvnitr. */
    {
        const char *tc = getenv("ELF_LOADER_TRACE_CALL");
        if (tc && tc[0]) {
            char buf[512];
            strncpy(buf, tc, sizeof buf - 1);
            buf[sizeof buf - 1] = '\0';
            for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                unsigned long addr = strtoul(tok, NULL, 0);
                if (addr) install_call_trace((void *)addr);
            }
        }
    }
    /* ELF_LOADER_PAUSE_CALL=0xADDR[,...]: jako TRACE_CALL, ale navic uspi
     * 30s (viz trace_call_pause_logger) - cili na CALL SITE (blr), napr.
     * v8::internal::Invoke->JSEntryTrampoline (0xde9854), PRED bootstrap
     * JS - nezavisle na --inspect-brk chovani InterpreterEntryTrampoline. */
    {
        const char *pc = getenv("ELF_LOADER_PAUSE_CALL");
        if (pc && pc[0]) {
            char buf[512];
            strncpy(buf, pc, sizeof buf - 1);
            buf[sizeof buf - 1] = '\0';
            for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                unsigned long addr = strtoul(tok, NULL, 0);
                if (addr) install_pause_call_trace((void *)addr);
            }
        }
    }
    /* ELF_LOADER_TRACE_ENTRY=0xADDR[,...]: jako vyse, ale pro instrukce
     * ktere nejsou `blr` (viz install_entry_trace) - loguje x1(JSFunction)
     * a x30(volajici) pri KAZDEM vstupu do dane funkce, napr. entry do
     * Builtins_InterpreterEntryTrampoline pred padem. */
    {
        const char *tr = getenv("ELF_LOADER_TRACE_REGS");
        if (tr && tr[0]) {
            char buf[512];
            strncpy(buf, tr, sizeof buf - 1);
            buf[sizeof buf - 1] = '\0';
            for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                unsigned long addr = strtoul(tok, NULL, 0);
                if (g_trace_peek < 0) g_trace_peek = getenv("ELF_LOADER_TRACE_PEEK") != NULL;
                if (addr) install_regs_trace_impl((void *)addr,
                                                  g_trace_peek ? (void *)trace_regs_peek_logger
                                                               : (void *)trace_regs_logger,
                                                  "TRACE_REGS", 0);
            }
        }
    }
    {
        const char *te = getenv("ELF_LOADER_TRACE_ENTRY");
        if (te && te[0]) {
            char buf[512];
            strncpy(buf, te, sizeof buf - 1);
            buf[sizeof buf - 1] = '\0';
            for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                unsigned long addr = strtoul(tok, NULL, 0);
                if (addr) install_entry_trace((void *)addr);
            }
        }
    }
    /* ELF_LOADER_TRACE_RING=0xADDR[,...]: jako TRACE_CALL, ale bez souboroveho
     * zapisu v hot path - ulozi x0-x2 do kruhoveho bufferu (viz ring_logger),
     * vypis az z fault_handleru (elf_dispatch_ring_dump). Pro mista volana
     * na kazdy bytecode (interpreter dispatch loop). */
    {
        const char *tr = getenv("ELF_LOADER_TRACE_RING");
        if (tr && tr[0]) {
            char buf[512];
            strncpy(buf, tr, sizeof buf - 1);
            buf[sizeof buf - 1] = '\0';
            for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                unsigned long addr = strtoul(tok, NULL, 0);
                if (addr) install_ring_trace((void *)addr);
            }
        }
    }
    /* ELF_LOADER_PAUSE_ENTRY=0xADDR[,...]: jako TRACE_ENTRY, ale navic uspi
     * bezi vlakno na 30s (viz install_pause_entry_trace/trace_entry_pause_
     * logger) - okno pro pripojeni externiho debuggeru (trepan-ni pres
     * --inspect-brk) bez zavodeni o cas. */
    {
        const char *pe = getenv("ELF_LOADER_PAUSE_ENTRY");
        if (pe && pe[0]) {
            char buf[512];
            strncpy(buf, pe, sizeof buf - 1);
            buf[sizeof buf - 1] = '\0';
            for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                unsigned long addr = strtoul(tok, NULL, 0);
                if (addr) install_pause_entry_trace((void *)addr);
            }
        }
    }
    /* ELF_LOADER_TRACE_STRROOT=0xADDR: jednorazovy experiment - overeni
     * hypotezy o posunutem base pointeru interniho string-rootu (viz
     * install_strroot_probe/strroot_logger). */
    {
        const char *ts = getenv("ELF_LOADER_TRACE_STRROOT");
        if (ts && ts[0]) {
            unsigned long addr = strtoul(ts, NULL, 0);
            if (addr) install_strroot_probe((void *)addr);
        }
    }

    g_exe_base = (uintptr_t)obj->base_addr;
    scope->exe = obj;   /* fallback pro symboly z hlavniho exe (PyExc_*, _PyRuntime) */
    ldso_install_exe_linkmap(obj, path);
    ldso_install_module_list(scope->mods, scope->count);

    /* App seccomp KILLuje rseq (293), set_robust_list (99, delka 24) a
     * clone3 (435). KILL obchazi SIGSYS handler i nas ERRNO filtr, proto v
     * kazdem nactenem modulu prebijeme `svc #0` techto syscallu. Pro 99/293
     * staci NOP (syscall se neprovede). U clone3 potrebujeme vratit -ENOSYS,
     * aby Rust/glibc fallbacknul na clone() - jinak by NOP znamenal, ze se
     * navratova hodnota x0 nikdy nenastavi a vlakno by se nespravne vytvorilo.
     * Tyká se hlavne libc (__tls_init_tp, start_thread, _Fork), ale projdeme
     * vsechny moduly. */
    {
        static const struct { long nr; long err; } kill_syscalls[] = {
            { 99,  0 },      /* set_robust_list -> NOP */
            { 293, 0 },      /* rseq            -> NOP */
            { 435, 38 },     /* clone3          -> -ENOSYS */
            { 151, 0 },      /* setfsuid        -> NOP (app profil TRAPuje) */
            { 152, 0 },      /* setfsgid        -> NOP */
        };
        size_t nsc = sizeof(kill_syscalls) / sizeof(kill_syscalls[0]);
        if (!getenv("ELF_LOADER_NO_PATCH")) {
        for (size_t mi = 0; mi < scope->count; mi++) {
            for (size_t s = 0; s < nsc; s++)
                elf_patch_syscall_sites(scope->mods[mi], kill_syscalls[s].nr,
                                        kill_syscalls[s].err);
        }
        for (size_t s = 0; s < nsc; s++)
            elf_patch_syscall_sites(obj, kill_syscalls[s].nr,
                                    kill_syscalls[s].err);
        }
    }
    if (g_tls_trace)
        elf_dump_ldso_state(scope);
    if (getenv("ELF_LOADER_DUMP_MAPS")) {
        FILE *mf = fopen("/proc/self/maps", "r");
        if (mf) {
            char line[512];
            while (fgets(line, sizeof line, mf)) {
                if (strstr(line, "3000") || strstr(line, "heap") ||
                    strstr(line, "elf_loader") || strstr(line, "\\[stack\\]"))
                    fprintf(stderr, "  map: %s", line);
            }
            fclose(mf);
        }
    }
    elf_install_fault_handlers();
    g_loader_active = 0;  /* od ted bezi cilova binarka (parrot TLS) */
    elf_tls_ctx_t tls = elf_setup_own_tls(obj, scope);
    int ret = elf_run(obj, argc, argv, envp);
    elf_teardown_own_tls(&tls);
    elf_scope_destroy(scope);
    obj->scope = NULL;
    elf_unload(obj);
    return ret;
}

/* Inject a guest-only LD_PRELOAD (ELF_LOADER_PRELOAD) into the envp handed to
 * the own-loaded guest. bionic linker64 (which starts elf_loader itself) would
 * abort on a glibc .so in LD_PRELOAD ("CANNOT LINK EXECUTABLE ... libc.so.6
 * not found"), so the launcher passes the preload in a separate variable and
 * we turn it into LD_PRELOAD only for the guest ld.so. */
static char **elf_guest_envp(char **envp) {
    const char *preload = getenv("ELF_LOADER_PRELOAD");
    if (elf_debug())
        fprintf(stderr, "[dbg] elf_guest_envp: ELF_LOADER_PRELOAD=%s\n",
                preload ? preload : "(unset)");
    if (!preload || !preload[0]) return envp;

    int n = 0;
    for (int i = 0; envp[i]; i++) n++;
    char **ne = calloc((size_t)n + 2, sizeof(char *));
    if (!ne) return envp;

    int o = 0, have_preload = 0;
    char ld[4096];
    snprintf(ld, sizeof ld, "LD_PRELOAD=%s", preload);
    for (int i = 0; envp[i]; i++) {
        if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0) {
            ne[o++] = strdup(ld);
            have_preload = 1;
            continue;
        }
        ne[o++] = envp[i];
    }
    if (!have_preload) ne[o++] = strdup(ld);
    ne[o] = NULL;
    return ne;
}

/* Doslovny text puvodniho "elf_loader pomocnici" bloku z host .zshrc
 * (lxwhich/lx/lxq/lxdbg/lxhelper/lxlog/lxdiag/lxinfo/lxfault/lxtest/
 * _lxtest_tmux/command_not_found_handler/help), presunuty sem, aby .zshrc
 * mel jediny zdroj pravdy (`eval "$($L init zsh)"`) misto rucni kopie.
 * Zmena chovani lx nastroju = zmena tady + redeploy, ne uprava .zshrc. */
static const char LX_HELPERS_ZSH[] =
    "# ───────────────────────── elf_loader pomocníci ─────────────────────────\n"
    "# lxwhich <jméno|cesta>  → plná cesta guest binárky v Parrot rootfs\n"
    "lxwhich() {\n"
    "  local c=$1 d\n"
    "  if [[ $c == */* ]]; then\n"
    "    [[ $c == /* && $c != $R/* && $c != $D/* ]] && c=$R$c\n"
    "    [[ -e $c ]] && { print -r -- $c; return 0 }\n"
    "    return 1\n"
    "  fi\n"
    "  if [[ -n $VIRTUAL_ENV ]]; then\n"
    "    local ve=$VIRTUAL_ENV\n"
    "    [[ $ve == /* && $ve != $R/* && $ve != $D/* ]] && ve=$R$ve\n"
    "    [[ -x $ve/bin/$c && ! -d $ve/bin/$c ]] && { print -r -- $ve/bin/$c; return 0 }\n"
    "  fi\n"
    "  for d in $LX_PATH; do\n"
    "    [[ -x $d/$c && ! -d $d/$c ]] && { print -r -- $d/$c; return 0 }\n"
    "  done\n"
    "  return 1\n"
    "}\n"
    "\n"
    "# lx <příkaz> [args]  → spustí Parrot binárku pod loaderem (--ownall)\n"
    "# Proměnné: LX_SHELL (SHELL pro guest), LX_ENV=\"A=1 B=2\" (extra env)\n"
    "# Guest PATH se nastaví na cesty v rootfs (gcc potřebuje najít ld/as přes PATH).\n"
    "lx() {\n"
    "  (( $# )) || { print -u2 \"použití: lx <příkaz> [args]\"; return 2 }\n"
    "  local bin\n"
    "  bin=$(lxwhich $1) || { print -u2 \"lx: '$1' není v Parrot rootfs\"; return 127 }\n"
    "  shift\n"
    "  local ve=\n"
    "  if [[ -n $VIRTUAL_ENV ]]; then\n"
    "    ve=$VIRTUAL_ENV\n"
    "    [[ $ve == /* && $ve != $R/* && $ve != $D/* ]] && ve=$R$ve\n"
    "  fi\n"
    "  local GPATH=${ve:+$ve/bin:}$R/usr/local/sbin:$R/usr/local/bin:$R/usr/sbin:$R/usr/bin:$R/sbin:$R/bin\n"
    "  env PATH=$GPATH LC_ALL=${LC_ALL:-C.UTF-8} SHELL=${LX_SHELL:-$R/bin/bash} ${=LX_ENV} \\\n"
    "      $L --ownall $bin \"$@\"\n"
    "}\n"
    "\n"
    "# lxq → jako lx, bez [MMAP] šumu ve stderr (jen neinteraktivně)\n"
    "lxq() { lx \"$@\" 2> >(grep -v '^\\[MMAP\\]' >&2) }\n"
    "\n"
    "# lxdbg VAR=1 <příkaz> → běh s ladicí proměnnou loaderu\n"
    "lxdbg() { local e=$1; shift; LX_ENV=\"$LX_ENV $e\" lx \"$@\" }\n"
    "\n"
    "# lxhelper <cesta.so> <příkaz> → běh s vlastní glibc helper knihovnou\n"
    "lxhelper() { local h=$1; shift; LX_ENV=\"$LX_ENV ELF_LOADER_HELPER=$h\" lx \"$@\" }\n"
    "\n"
    "# lxlog <příkaz> → výstup do logu, na konci shrnutí\n"
    "lxlog() {\n"
    "  local f=$LX_LOG/${1:t}-$(date +%H%M%S).log\n"
    "  lx \"$@\" > $f 2>&1\n"
    "  local st=$?\n"
    "  grep -v '^\\[MMAP\\]' $f | tail -20\n"
    "  print -P \"%F{8}── exit=$st  log: $f%f\"\n"
    "  return $st\n"
    "}\n"
    "\n"
    "# lxdiag [-c] → diag.txt loaderu (SIGSYS/INIT stopy), -c smaže\n"
    "lxdiag() {\n"
    "  local f=$D/usr/diag.txt\n"
    "  [[ $1 == -c ]] && { : > $f; print \"diag.txt vyčištěn\"; return }\n"
    "  [[ -s $f ]] && tail -40 $f || print \"diag.txt je prázdný\"\n"
    "}\n"
    "\n"
    "# lxinfo → verze a stav loaderu\n"
    "lxinfo() {\n"
    "  print \"loader : $L\"\n"
    "  [[ -x $L ]] && ls -l $L | awk '{print \"         \" $5 \" B, \" $6 \" \" $7}' || print \"         CHYBÍ\"\n"
    "  print \"rootfs : $R\"\n"
    "  local h=$R/root/elf_loader/.git/HEAD\n"
    "  [[ -r $h ]] && print \"repo   : $(<$h)\"\n"
    "  print \"locale : LOCPATH=$LOCPATH\"\n"
    "}\n"
    "\n"
    "# lxfault <příkaz> → jen řádky s pádem\n"
    "lxfault() {\n"
    "  lx \"$@\" 2>&1 | grep -E 'FAULT#|pc in:|SIGSYS|panic|Segmentation|Aborted|\\[-\\]' | head -20\n"
    "}\n"
    "\n"
    "# lxtest → regresní sada (PASS / FLAKY / FAIL / SKIP), logy v $LX_LOG/test\n"
    "lxtest() {\n"
    "  local dir=$LX_LOG/test; mkdir -p $dir\n"
    "  local claude=$R/root/claudetest/claude.exe\n"
    "  local -a names cmds expect needs\n"
    "  local n22=$R/root/.nvm/versions/node/v22.11.0/bin/node\n"
    "  names=(true echo bash-42 exec-chain python3 node22 node-latest bun-claude tmux-V tmux-session)\n"
    "  cmds=(\n"
    "    'lx true'\n"
    "    'lx echo lx_ok'\n"
    "    'lx bash -c \"echo \\$((6*7))\"'\n"
    "    \"lx env $R/bin/bash -c 'echo chain_ok'\"\n"
    "    'lx python3 -c \"print(6*7)\" </dev/null'\n"
    "    \"lx $n22 -e 'console.log(6*7)'\"\n"
    "    \"lx node -e 'console.log(6*7)'\"\n"
    "    \"env LC_ALL=C.UTF-8 $L --ownall $claude --version\"\n"
    "    'lx tmux -V'\n"
    "    '_lxtest_tmux'\n"
    "  )\n"
    "  expect=('' lx_ok 42 chain_ok 42 42 42 'Claude Code' 'tmux 3' AHOJ_42)\n"
    "  needs=(true echo bash bash python3 $n22 node $claude tmux tmux)\n"
    "  local i st out t0 dt try sig first pass=0 fail=0 flaky=0 skip=0 res\n"
    "  [[ -x $L ]] || { print -u2 \"loader $L chybí\"; return 1 }\n"
    "  print -P \"%F{8}loader: $L ($(ls -l $L | awk '{print $5\" B \"$6\" \"$7}'))%f\"\n"
    "  for i in {1..$#names}; do\n"
    "    if ! lxwhich ${needs[i]} >/dev/null; then\n"
    "      print -P \"%F{8}SKIP %f  ${(r:14:)names[i]} chybí ${needs[i]:t}\"; (( skip++ )); continue\n"
    "    fi\n"
    "    res=FAIL\n"
    "    for try in 1 2; do\n"
    "      t0=$EPOCHREALTIME\n"
    "      timeout 90 zsh -c \"$(typeset -p LX_PATH); $(functions lx lxwhich _lxtest_tmux); ${cmds[i]}\" > $dir/${names[i]}.$try.log 2>&1\n"
    "      st=$?\n"
    "      dt=$(printf '%.1f' $(( EPOCHREALTIME - t0 )))\n"
    "      # bez [MMAP] šumu a bez výpisu fault handleru (hex adresy dávají falešné shody)\n"
    "      out=$(grep -vE '^\\[MMAP\\]|^FAULT#|^  |^[0-9a-f]+-[0-9a-f]+ ' $dir/${names[i]}.$try.log)\n"
    "      sig=\"\"\n"
    "      (( st > 128 && st < 128 + ${#signals} )) && sig=\" SIG${signals[st-128+1]}\"\n"
    "      # pád na SEGV/BUS/ILL/FPE/SYS je vždy FAIL; ABRT (teardown node) je povolený\n"
    "      if (( st == 139 || st == 135 || st == 132 || st == 136 || st == 159 )); then\n"
    "        :\n"
    "      elif [[ -z ${expect[i]} && $st == 0 ]] || [[ -n ${expect[i]} && $out == *${expect[i]}* ]]; then\n"
    "        (( try == 1 )) && res=PASS || res=FLAKY\n"
    "        break\n"
    "      fi\n"
    "      (( try == 1 )) && first=\"exit=$st$sig\"\n"
    "    done\n"
    "    case $res in\n"
    "      PASS)  print -P \"%F{green}PASS %f  ${(r:14:)names[i]} exit=$st$sig  ${dt}s\"; (( pass++ )) ;;\n"
    "      FLAKY) print -P \"%F{yellow}FLAKY%f  ${(r:14:)names[i]} 1. pokus $first, 2. prošel  → $dir/${names[i]}.1.log\"; (( flaky++ )) ;;\n"
    "      FAIL)  print -P \"%F{red}FAIL %f  ${(r:14:)names[i]} exit=$st$sig  ${dt}s  → $dir/${names[i]}.2.log\"; (( fail++ )) ;;\n"
    "    esac\n"
    "  done\n"
    "  print -P \"── %F{green}$pass PASS%f  %F{yellow}$flaky FLAKY%f  %F{red}$fail FAIL%f  %F{8}$skip SKIP%f\"\n"
    "}\n"
    "_lxtest_tmux() {\n"
    "  local s=$R/tmp/lxtest.sock; rm -f $s\n"
    "  LX_SHELL=$R/bin/bash lx tmux -S $s new-session -d -s t -x 80 -y 24\n"
    "  sleep 3\n"
    "  lx tmux -S $s send-keys -t t 'echo AHOJ_$((6*7))' Enter; sleep 2\n"
    "  lx tmux -S $s capture-pane -p -t t\n"
    "  lx tmux -S $s kill-server; rm -f $s\n"
    "}\n"
    "\n"
    "# lxfb [-m \"poznámka\"] <příkaz> [args] → běh s plným logováním loaderu a\n"
    "# feedback report (Markdown) pro PR do dev/feedback/: datum, zařízení, Android,\n"
    "# kernel, page size, loader, rootfs OS + glibc, příkaz, exit/signál, výpis\n"
    "# pádu, celý log a nové diag.<pid>.txt. Syntaxe společná pro zsh i bash.\n"
    "# Proměnné: LX_FEEDBACK (adresář reportů, default $LX_LOG/feedback),\n"
    "# LXFB_DEBUG (debug env loaderu), LXFB_MAX (max řádků logu v reportu).\n"
    "lxfb() {\n"
    "  local note=\n"
    "  if [ \"$1\" = -m ]; then note=$2; shift 2; fi\n"
    "  [ $# -gt 0 ] || { printf 'použití: lxfb [-m \"poznámka\"] <příkaz> [args]\\n' >&2; return 2; }\n"
    "  local dir=${LX_FEEDBACK:-$LX_LOG/feedback}\n"
    "  local dbg=${LXFB_DEBUG:-ELF_DEBUG=1 ELF_LOADER_DIAG=1 ELF_LOADER_SIGTRACE=1}\n"
    "  local max=${LXFB_MAX:-4000}\n"
    "  mkdir -p \"$dir\" || return 1\n"
    "  local model=$(getprop ro.product.model 2>/dev/null)\n"
    "  local dev=$(printf '%s' \"${model:-device}\" | tr 'A-Z' 'a-z' | tr -c 'a-z0-9\\n' '-')\n"
    "  local cmdn=$(printf '%s' \"${1##*/}\" | tr -c 'A-Za-z0-9._\\n' '-')\n"
    "  local stamp=$(date +%Y-%m-%d)\n"
    "  local base=$dir/$dev-$stamp-$cmdn-$(date +%H%M%S)\n"
    "  local log=$base.log rcf=$base.rc out=$base.md mark=$base.mark\n"
    "  local libc= f\n"
    "  for f in $R/usr/lib/aarch64-linux-gnu/libc.so.6 $R/lib/aarch64-linux-gnu/libc.so.6 $R/usr/lib64/libc.so.6; do\n"
    "    [ -f \"$f\" ] && { libc=$(grep -a -o -m1 'GNU C Library ([^)]*) [a-z ]*version [0-9.]*' \"$f\" | head -1); break; }\n"
    "  done\n"
    "  local os=$(sed -n 's/^PRETTY_NAME=\"\\{0,1\\}\\([^\"]*\\)\"\\{0,1\\}$/\\1/p' \"$R/etc/os-release\" 2>/dev/null)\n"
    "  local cmdq=$(printf '%q ' \"$@\")\n"
    "  cmdq=${cmdq% }\n"
    "  : > \"$mark\"\n"
    "  local t0=$(date +%s)\n"
    "  printf '── lxfb: %s\\n' \"$cmdq\" >&2\n"
    "  { LX_ENV=\"$LX_ENV $dbg\" lx \"$@\"; echo $? > \"$rcf\"; } 2>&1 | tee \"$log\"\n"
    "  local st=$(cat \"$rcf\" 2>/dev/null) t1=$(date +%s)\n"
    "  st=${st:-?}\n"
    "  local sig=\n"
    "  if [ \"$st\" != '?' ] && [ \"$st\" -gt 128 ] 2>/dev/null; then sig=\" (SIG$(kill -l $((st - 128)) 2>/dev/null))\"; fi\n"
    "  local n=$(wc -l < \"$log\" | tr -d ' ')\n"
    "  local diags=$(find \"$D/usr\" -maxdepth 1 -name 'diag.*.txt' -newer \"$mark\" 2>/dev/null)\n"
    "  {\n"
    "    printf '# elf_loader feedback: `%s`\\n\\n' \"$cmdq\"\n"
    "    [ -n \"$note\" ] && printf '> %s\\n\\n' \"$note\"\n"
    "    printf -- '- Date: %s\\n' \"$(date '+%Y-%m-%d %H:%M:%S %z')\"\n"
    "    printf -- '- Device / SoC: %s %s (%s)\\n' \"$(getprop ro.product.manufacturer 2>/dev/null)\" \"$model\" \"$(getprop ro.board.platform 2>/dev/null)\"\n"
    "    printf -- '- Android: %s (SDK %s)\\n' \"$(getprop ro.build.version.release 2>/dev/null)\" \"$(getprop ro.build.version.sdk 2>/dev/null)\"\n"
    "    printf -- '- Kernel: %s\\n' \"$(uname -srm)\"\n"
    "    printf -- '- Page size: %s\\n' \"$(getconf PAGESIZE 2>/dev/null)\"\n"
    "    printf -- '- su available: %s\\n' \"$(command -v su >/dev/null 2>&1 && echo yes || echo no)\"\n"
    "    printf -- '- elf_loader: %s, %s B, md5 %s\\n' \"$(\"$L\" --version 2>/dev/null | head -1)\" \"$(wc -c < \"$L\" | tr -d ' ')\" \"$(md5sum \"$L\" 2>/dev/null | cut -c1-12)\"\n"
    "    printf -- '- Rootfs: %s; %s\\n' \"${os:-?}\" \"${libc:-glibc ?}\"\n"
    "    printf -- '- Debug env: `%s`\\n\\n' \"$dbg $LX_ENV\"\n"
    "    printf '| command | exit | duration |\\n|---|---|---|\\n| `%s` | %s%s | %ss |\\n\\n' \"$cmdq\" \"$st\" \"$sig\" \"$((t1 - t0))\"\n"
    "    printf '## Crash lines\\n\\n~~~~\\n'\n"
    "    grep -E 'FAULT#|pc in:|SIGSYS|panic|Segmentation|Aborted|SyntaxError|\\[-\\]|FATAL' \"$log\" | grep -v '^\\[sig\\] SIGSYS cur=' | head -40\n"
    "    printf '~~~~\\n\\n## Log (%s lines' \"$n\"\n"
    "    if [ \"$n\" -gt \"$max\" ]; then\n"
    "      printf ', first 500 + last %s; full log: %s)\\n\\n~~~~\\n' \"$((max - 500))\" \"$log\"\n"
    "      head -500 \"$log\"; printf '\\n… %s lines skipped …\\n\\n' \"$((n - max))\"; tail -n \"$((max - 500))\" \"$log\"\n"
    "    else\n"
    "      printf ')\\n\\n~~~~\\n'; cat \"$log\"\n"
    "    fi\n"
    "    printf '~~~~\\n'\n"
    "    [ -n \"$diags\" ] && printf '%s\\n' \"$diags\" | while IFS= read -r f; do\n"
    "      printf '\\n## %s\\n\\n~~~~\\n' \"${f##*/}\"; tail -n 200 \"$f\"; printf '~~~~\\n'\n"
    "    done\n"
    "  } > \"$out\"\n"
    "  rm -f \"$rcf\" \"$mark\"\n"
    "  printf -- '── exit=%s%s  report: %s\\n' \"$st\" \"$sig\" \"$out\" >&2\n"
    "  printf -- '   před odesláním zkontroluj obsah (cesty, argumenty); pak ho zkopíruj do feedback/ ve forku a pošli PR do dev\\n' >&2\n"
    "  [ \"$st\" = '?' ] && return 1\n"
    "  return $st\n"
    "}\n"
    "\n"
    "# Neznámý příkaz → zkusí ho najít v Parrot rootfs a spustit přes loader.\n"
    "command_not_found_handler() {\n"
    "  local bin\n"
    "  if bin=$(lxwhich $1 2>/dev/null); then\n"
    "    [[ $1 == (starship|zoxide) ]] || print -u2 -P \"%F{8}[lx] $1 → ${bin#$R}%f\"\n"
    "    lx \"$@\"\n"
    "    return $?\n"
    "  fi\n"
    "  print -u2 \"zsh: příkaz nenalezen: $1\"\n"
    "  return 127\n"
    "}\n"
    "\n"
    "# help [topic] → help with examples\n"
    "help() {\n"
    "  local c=$'\\e[36m' y=$'\\e[33m' g=$'\\e[90m' b=$'\\e[1m' r=$'\\e[0m'\n"
    "  local t=${1:-overview}\n"
    "  _h_title() { print \"\\n${b}${c}── $1 ──${r}\" }\n"
    "  _h_cmd()   { printf \"  ${y}%-22s${r} %s\\n\" \"$1\" \"$2\" }\n"
    "  _h_ex()    { print \"    ${g}\\$${r} $1\" }\n"
    "\n"
    "  if [[ $t == overview || $t == all ]]; then\n"
    "    print \"${b}Help for the elf_loader shell${r}\"\n"
    "    print \"Variables: ${y}\\$D${r} = app files (HOME), ${y}\\$R${r} = Parrot rootfs, ${y}\\$L${r} = loader\"\n"
    "    print \"Details: ${y}help lx${r} | ${y}help test${r} | ${y}help debug${r} | ${y}help helper${r} | ${y}help tmux${r} | ${y}help prompt${r} | ${y}help keys${r} | ${y}help all${r}\"\n"
    "  fi\n"
    "\n"
    "  if [[ $t == lx || $t == all || $t == overview ]]; then\n"
    "    _h_title \"Running Parrot programs through the loader\"\n"
    "    _h_cmd \"lx <cmd> [arg]\" \"run a Parrot binary under the loader (searched in Parrot PATH)\"\n"
    "    _h_cmd \"lxq <cmd> [arg]\" \"same, but without [MMAP] noise (non-interactive only)\"\n"
    "    _h_cmd \"lxwhich <cmd>\" \"show where a binary lives in the Parrot rootfs\"\n"
    "    _h_cmd \"<unknown command>\" \"tries to find it in Parrot and run it via the loader\"\n"
    "    if [[ $t != overview ]]; then\n"
    "      print \"  Examples:\"\n"
    "      _h_ex \"lx bash -c 'echo \\$((6*7))'        ${g}# Parrot bash, prints 42${r}\"\n"
    "      _h_ex \"lx python3 -c 'import sys; print(sys.version)'\"\n"
    "      _h_ex \"git --version                     ${g}# not on host -> runs Parrot git${r}\"\n"
    "      _h_ex \"lxwhich node                      ${g}# node from nvm in the rootfs${r}\"\n"
    "      _h_ex \"lxq ls -la /etc | head            ${g}# without [MMAP] lines${r}\"\n"
    "      _h_ex \"LX_ENV='FOO=1 BAR=2' lx env | grep -E 'FOO|BAR'   ${g}# extra vars for guest${r}\"\n"
    "    fi\n"
    "  fi\n"
    "\n"
    "  if [[ $t == test || $t == all || $t == overview ]]; then\n"
    "    _h_title \"Tests\"\n"
    "    _h_cmd \"lxtest\" \"regression set: echo, bash, exec, python, node22, latest node, bun, tmux\"\n"
    "    _h_cmd \"lxlog <cmd>\" \"run with output to a log, shows the tail and exit code\"\n"
    "    _h_cmd \"lxinfo\" \"loader size/date, rootfs, repo branch\"\n"
    "    _h_cmd \"lxfb [-m note] <cmd>\" \"full-debug run + feedback report (device, Android, rootfs, glibc, log)\"\n"
    "    if [[ $t != overview ]]; then\n"
    "      print \"  Results: ${g}PASS${r} passed, ${y}FLAKY${r} passed on 2nd try, FAIL failed twice,\"\n"
    "      print \"  SKIP binary missing in rootfs. Logs are in ${y}\\$LX_LOG/test${r} (~/.cache/lx/test).\"\n"
    "      print \"  Examples:\"\n"
    "      _h_ex \"lxtest\"\n"
    "      _h_ex \"L=\\$D/usr/bin/other_loader lxtest   ${g}# same set with another loader (A/B)${r}\"\n"
    "      _h_ex \"lxlog node -e 'console.log(1)'\"\n"
    "      _h_ex \"lxfb -m 'crash on start' claude   ${g}# report in \\$LX_LOG/feedback -> PR to dev/feedback/${r}\"\n"
    "      _h_ex \"for i in {1..50}; do lxq echo x >/dev/null || print crash \\$?; done   ${g}# crash rate${r}\"\n"
    "    fi\n"
    "  fi\n"
    "\n"
    "  if [[ $t == debug || $t == all || $t == overview ]]; then\n"
    "    _h_title \"Debugging crashes\"\n"
    "    _h_cmd \"lxfault <cmd>\" \"print only crash lines (FAULT, pc in, SIGSYS, panic)\"\n"
    "    _h_cmd \"lxdbg VAR=1 <cmd>\" \"run with a loader debug variable\"\n"
    "    _h_cmd \"lxdiag [-c]\" \"loader diag.txt with SIGSYS and startup traces, -c clears it\"\n"
    "    if [[ $t != overview ]]; then\n"
    "      print \"  Useful variables: ${y}ELF_DEBUG=1${r} (load progress), ${y}ELF_LOADER_SIGTRACE=1${r},\"\n"
    "      print \"  ${y}ELF_LOADER_VMTRACE=1${r} (all mmaps), ${y}ELF_LOADER_DUMP_MAPS=1${r} (memory map).\"\n"
    "      print \"  Examples:\"\n"
    "      _h_ex \"lxfault node --version\"\n"
    "      _h_ex \"lxdiag -c; lx bash -i; lxdiag      ${g}# what the SIGSYS handler caught during bash${r}\"\n"
    "      _h_ex \"lxdbg ELF_LOADER_SIGTRACE=1 bash -c true\"\n"
    "    fi\n"
    "  fi\n"
    "\n"
    "  if [[ $t == helper || $t == all || $t == overview ]]; then\n"
    "    _h_title \"Helper libraries (ELF_LOADER_HELPER)\"\n"
    "    _h_cmd \"lxhelper <.so> <cmd>\" \"run a command with a custom glibc library ahead of libc\"\n"
    "    if [[ $t != overview ]]; then\n"
    "      print \"  The library is built in Parrot with plain gcc and overrides libc functions called via PLT.\"\n"
    "      print \"  It finds the real function via dlsym(RTLD_NEXT). Functions the loader itself overrides\"\n"
    "      print \"  (open, stat, exec...) take precedence. Multiple libraries: colon-separated paths.\"\n"
    "      print \"  Examples:\"\n"
    "      _h_ex \"lxhelper \\$R/root/elf_loader/helper/lxhelper_test.so uname -r   ${g}# release ends with +lxhelper${r}\"\n"
    "      _h_ex \"LX_ENV='LXHELPER_VERBOSE=1' lxhelper \\$R/root/elf_loader/helper/lxhelper_test.so true\"\n"
    "    fi\n"
    "  fi\n"
    "\n"
    "  if [[ $t == tmux || $t == all ]]; then\n"
    "    _h_title \"tmux\"\n"
    "    _h_cmd \"ltmux [arg]\" \"Parrot tmux via the loader, shell = bionic zsh\"\n"
    "    print \"  Examples:\"\n"
    "    _h_ex \"ltmux                             ${g}# new session${r}\"\n"
    "    _h_ex \"ltmux attach                      ${g}# attach to a running session${r}\"\n"
    "    _h_ex \"LTMUX_SHELL=\\$R/bin/bash ltmux     ${g}# Parrot bash instead of zsh${r}\"\n"
    "  fi\n"
    "\n"
    "  if [[ $t == prompt || $t == all ]]; then\n"
    "    _h_title \"Prompt\"\n"
    "    print \"  ${c}host${r} ${b}~/path${r} ${g}(dev)${r} ${g}3.2s${r} \\e[31m✗139 SIGSEGV${r}\"\n"
    "    print \"  The branch shows only in the loader repo, duration only above 2 s, exit code only on error.\"\n"
    "  fi\n"
    "\n"
    "  if [[ $t == keys || $t == all ]]; then\n"
    "    _h_title \"Keys and plugins\"\n"
    "    _h_cmd \"Up/Down\" \"search history by the text typed\"\n"
    "    _h_cmd \"Right / End\" \"accept the grey history suggestion\"\n"
    "    _h_cmd \"Tab\" \"completion with a menu, case-insensitive\"\n"
    "    _h_cmd \"Ctrl+U\" \"delete the line before the cursor\"\n"
    "    _h_cmd \"Ctrl+arrows\" \"jump by words\"\n"
    "    _h_cmd \"Shift+Tab\" \"undo the last edit\"\n"
    "  fi\n"
    "\n"
    "  if [[ $t == overview ]]; then\n"
    "    _h_title \"Other\"\n"
    "    _h_cmd \"cdl / cdr\" \"go to the loader repo / the Parrot rootfs\"\n"
    "    _h_cmd \"reload\" \"reload ~/.zshrc\"\n"
    "  fi\n"
    "  unfunction _h_title _h_cmd _h_ex\n"
    "  [[ $t == (overview|all|lx|test|debug|helper|tmux|prompt|keys) ]] || { print -u2 \"unknown topic: $t\"; return 1 }\n"
    "}\n"
    "alias lxhelp=help\n";

/* Bash varianta pomocniku (stejna jmena jako zsh blok, prepsana do bash
 * syntaxe). Pouziva se v .bashrc pres `eval "$($L init bash)"`. */
static const char LX_HELPERS_BASH[] =
    "# ───────────────────────── elf_loader pomocníci (bash) ─────────────────────────\n"
    "lxwhich() {\n"
    "  local c=$1 d\n"
    "  if [[ $c == */* ]]; then\n"
    "    [[ $c == /* && $c != $R/* && $c != $D/* ]] && c=$R$c\n"
    "    [[ -e $c ]] && { printf '%s\\n' \"$c\"; return 0; }\n"
    "    return 1\n"
    "  fi\n"
    "  if [ -n \"$VIRTUAL_ENV\" ]; then\n"
    "    local ve=$VIRTUAL_ENV\n"
    "    [[ $ve == /* && $ve != $R/* && $ve != $D/* ]] && ve=$R$ve\n"
    "    [[ -x $ve/bin/$c && ! -d $ve/bin/$c ]] && { printf '%s\\n' \"$ve/bin/$c\"; return 0; }\n"
    "  fi\n"
    "  for d in $LX_PATH; do\n"
    "    [[ -x $d/$c && ! -d $d/$c ]] && { printf '%s\\n' \"$d/$c\"; return 0; }\n"
    "  done\n"
    "  return 1\n"
    "}\n"
    "\n"
    "lx() {\n"
    "  (( $# )) || { printf 'použití: lx <příkaz> [args]\\n' >&2; return 2; }\n"
    "  local bin\n"
    "  bin=$(lxwhich \"$1\") || { printf \"lx: '%s' není v rootfs\\n\" \"$1\" >&2; return 127; }\n"
    "  shift\n"
    "  local ve=\n"
    "  if [ -n \"$VIRTUAL_ENV\" ]; then\n"
    "    ve=$VIRTUAL_ENV\n"
    "    [[ $ve == /* && $ve != $R/* && $ve != $D/* ]] && ve=$R$ve\n"
    "  fi\n"
    "  local GPATH=${ve:+$ve/bin:}$R/usr/local/sbin:$R/usr/local/bin:$R/usr/sbin:$R/usr/bin:$R/sbin:$R/bin\n"
    "  env PATH=$GPATH LC_ALL=${LC_ALL:-C.UTF-8} SHELL=${LX_SHELL:-$R/bin/bash} ${LX_ENV} \\\n"
    "      \"$L\" --ownall \"$bin\" \"$@\"\n"
    "}\n"
    "\n"
    "lxq() { lx \"$@\" 2> >(grep -v '^\\[MMAP\\]' >&2); }\n"
    "\n"
    "lxlog() {\n"
    "  local f=$LX_LOG/${1##*/}-$(date +%H%M%S).log\n"
    "  lx \"$@\" > \"$f\" 2>&1\n"
    "  local st=$?\n"
    "  grep -v '^\\[MMAP\\]' \"$f\" | tail -20\n"
    "  printf -- '── exit=%s  log: %s\\n' \"$st\" \"$f\"\n"
    "  return $st\n"
    "}\n"
    "\n"
    "lxinfo() {\n"
    "  printf 'loader : %s\\n' \"$L\"\n"
    "  [ -x \"$L\" ] && ls -l \"$L\" | awk '{print \"         \" $5 \" B, \" $6 \" \" $7}' || printf '         CHYBÍ\\n'\n"
    "  printf 'rootfs : %s\\n' \"$R\"\n"
    "  printf 'locale : LOCPATH=%s\\n' \"$LOCPATH\"\n"
    "}\n"
    "\n"
    "lxfault() {\n"
    "  lx \"$@\" 2>&1 | grep -E 'FAULT#|pc in:|SIGSYS|panic|Segmentation|Aborted|\\[-\\]' | head -20\n"
    "}\n"
    "\n"
    "# lxfb [-m \"poznámka\"] <příkaz> [args] → běh s plným logováním loaderu a\n"
    "# feedback report (Markdown) pro PR do dev/feedback/: datum, zařízení, Android,\n"
    "# kernel, page size, loader, rootfs OS + glibc, příkaz, exit/signál, výpis\n"
    "# pádu, celý log a nové diag.<pid>.txt. Syntaxe společná pro zsh i bash.\n"
    "# Proměnné: LX_FEEDBACK (adresář reportů, default $LX_LOG/feedback),\n"
    "# LXFB_DEBUG (debug env loaderu), LXFB_MAX (max řádků logu v reportu).\n"
    "lxfb() {\n"
    "  local note=\n"
    "  if [ \"$1\" = -m ]; then note=$2; shift 2; fi\n"
    "  [ $# -gt 0 ] || { printf 'použití: lxfb [-m \"poznámka\"] <příkaz> [args]\\n' >&2; return 2; }\n"
    "  local dir=${LX_FEEDBACK:-$LX_LOG/feedback}\n"
    "  local dbg=${LXFB_DEBUG:-ELF_DEBUG=1 ELF_LOADER_DIAG=1 ELF_LOADER_SIGTRACE=1}\n"
    "  local max=${LXFB_MAX:-4000}\n"
    "  mkdir -p \"$dir\" || return 1\n"
    "  local model=$(getprop ro.product.model 2>/dev/null)\n"
    "  local dev=$(printf '%s' \"${model:-device}\" | tr 'A-Z' 'a-z' | tr -c 'a-z0-9\\n' '-')\n"
    "  local cmdn=$(printf '%s' \"${1##*/}\" | tr -c 'A-Za-z0-9._\\n' '-')\n"
    "  local stamp=$(date +%Y-%m-%d)\n"
    "  local base=$dir/$dev-$stamp-$cmdn-$(date +%H%M%S)\n"
    "  local log=$base.log rcf=$base.rc out=$base.md mark=$base.mark\n"
    "  local libc= f\n"
    "  for f in $R/usr/lib/aarch64-linux-gnu/libc.so.6 $R/lib/aarch64-linux-gnu/libc.so.6 $R/usr/lib64/libc.so.6; do\n"
    "    [ -f \"$f\" ] && { libc=$(grep -a -o -m1 'GNU C Library ([^)]*) [a-z ]*version [0-9.]*' \"$f\" | head -1); break; }\n"
    "  done\n"
    "  local os=$(sed -n 's/^PRETTY_NAME=\"\\{0,1\\}\\([^\"]*\\)\"\\{0,1\\}$/\\1/p' \"$R/etc/os-release\" 2>/dev/null)\n"
    "  local cmdq=$(printf '%q ' \"$@\")\n"
    "  cmdq=${cmdq% }\n"
    "  : > \"$mark\"\n"
    "  local t0=$(date +%s)\n"
    "  printf '── lxfb: %s\\n' \"$cmdq\" >&2\n"
    "  { LX_ENV=\"$LX_ENV $dbg\" lx \"$@\"; echo $? > \"$rcf\"; } 2>&1 | tee \"$log\"\n"
    "  local st=$(cat \"$rcf\" 2>/dev/null) t1=$(date +%s)\n"
    "  st=${st:-?}\n"
    "  local sig=\n"
    "  if [ \"$st\" != '?' ] && [ \"$st\" -gt 128 ] 2>/dev/null; then sig=\" (SIG$(kill -l $((st - 128)) 2>/dev/null))\"; fi\n"
    "  local n=$(wc -l < \"$log\" | tr -d ' ')\n"
    "  local diags=$(find \"$D/usr\" -maxdepth 1 -name 'diag.*.txt' -newer \"$mark\" 2>/dev/null)\n"
    "  {\n"
    "    printf '# elf_loader feedback: `%s`\\n\\n' \"$cmdq\"\n"
    "    [ -n \"$note\" ] && printf '> %s\\n\\n' \"$note\"\n"
    "    printf -- '- Date: %s\\n' \"$(date '+%Y-%m-%d %H:%M:%S %z')\"\n"
    "    printf -- '- Device / SoC: %s %s (%s)\\n' \"$(getprop ro.product.manufacturer 2>/dev/null)\" \"$model\" \"$(getprop ro.board.platform 2>/dev/null)\"\n"
    "    printf -- '- Android: %s (SDK %s)\\n' \"$(getprop ro.build.version.release 2>/dev/null)\" \"$(getprop ro.build.version.sdk 2>/dev/null)\"\n"
    "    printf -- '- Kernel: %s\\n' \"$(uname -srm)\"\n"
    "    printf -- '- Page size: %s\\n' \"$(getconf PAGESIZE 2>/dev/null)\"\n"
    "    printf -- '- su available: %s\\n' \"$(command -v su >/dev/null 2>&1 && echo yes || echo no)\"\n"
    "    printf -- '- elf_loader: %s, %s B, md5 %s\\n' \"$(\"$L\" --version 2>/dev/null | head -1)\" \"$(wc -c < \"$L\" | tr -d ' ')\" \"$(md5sum \"$L\" 2>/dev/null | cut -c1-12)\"\n"
    "    printf -- '- Rootfs: %s; %s\\n' \"${os:-?}\" \"${libc:-glibc ?}\"\n"
    "    printf -- '- Debug env: `%s`\\n\\n' \"$dbg $LX_ENV\"\n"
    "    printf '| command | exit | duration |\\n|---|---|---|\\n| `%s` | %s%s | %ss |\\n\\n' \"$cmdq\" \"$st\" \"$sig\" \"$((t1 - t0))\"\n"
    "    printf '## Crash lines\\n\\n~~~~\\n'\n"
    "    grep -E 'FAULT#|pc in:|SIGSYS|panic|Segmentation|Aborted|SyntaxError|\\[-\\]|FATAL' \"$log\" | grep -v '^\\[sig\\] SIGSYS cur=' | head -40\n"
    "    printf '~~~~\\n\\n## Log (%s lines' \"$n\"\n"
    "    if [ \"$n\" -gt \"$max\" ]; then\n"
    "      printf ', first 500 + last %s; full log: %s)\\n\\n~~~~\\n' \"$((max - 500))\" \"$log\"\n"
    "      head -500 \"$log\"; printf '\\n… %s lines skipped …\\n\\n' \"$((n - max))\"; tail -n \"$((max - 500))\" \"$log\"\n"
    "    else\n"
    "      printf ')\\n\\n~~~~\\n'; cat \"$log\"\n"
    "    fi\n"
    "    printf '~~~~\\n'\n"
    "    [ -n \"$diags\" ] && printf '%s\\n' \"$diags\" | while IFS= read -r f; do\n"
    "      printf '\\n## %s\\n\\n~~~~\\n' \"${f##*/}\"; tail -n 200 \"$f\"; printf '~~~~\\n'\n"
    "    done\n"
    "  } > \"$out\"\n"
    "  rm -f \"$rcf\" \"$mark\"\n"
    "  printf -- '── exit=%s%s  report: %s\\n' \"$st\" \"$sig\" \"$out\" >&2\n"
    "  printf -- '   před odesláním zkontroluj obsah (cesty, argumenty); pak ho zkopíruj do feedback/ ve forku a pošli PR do dev\\n' >&2\n"
    "  [ \"$st\" = '?' ] && return 1\n"
    "  return $st\n"
    "}\n"
    "\n"
    "command_not_found_handle() {\n"
    "  local bin\n"
    "  if bin=$(lxwhich \"$1\" 2>/dev/null); then\n"
    "    [[ $1 == starship || $1 == zoxide ]] || printf '[lx] %s → %s\\n' \"$1\" \"${bin#$R}\" >&2\n"
    "    lx \"$@\"\n"
    "    return $?\n"
    "  fi\n"
    "  printf 'bash: %s: příkaz nenalezen\\n' \"$1\" >&2\n"
    "  return 127\n"
    "}\n";

/* `elf_loader init zsh` — stejny shell-integrace vzor jako `starship init zsh`/
 * `zoxide init zsh` (uz pouzivany v host .zshrc): vypise na stdout radky pro
 * `eval`, misto aby menil vlastni prostredi (setenv v tomto procesu by na
 * rodicovsky interaktivni shell nemel zadny vliv). Cte ROOTFS - musi byt uz
 * v env, kdyz se vola (typicky za `export ROOTFS=$R` v .zshrc). Stejne
 * defaulty jako v run_ownall() (LOCPATH/LC_ALL pro guest glibc pod tmuxem),
 * tady jen jako text k eval misto primeho setenv. SHELL zamerne vynechano -
 * host .zshrc ma vlastni, odlisny fallback (bionic zsh, ne rootfs bash).
 * Zaroven vypise cely LX_HELPERS_ZSH blok (lx/lxwhich/lxtest/help/...). */
/* Preamble sdileny pro zsh/bash: vsechny cesty se odvozuji z $ROOTFS/$HOME,
 * zadna absolutni cesta neni zadratovana. ROOTFS lze prebit; default je
 * $HOME/nh/distro/parrot (konvence app files diru). LOCPATH se nastavi jen
 * kdyz adresar v rootfs existuje (jinak by si glibc stezoval). */
static void elf_print_init_env(void) {
    fputs(
        "export D=${D:-$HOME}\n"
        "export ROOTFS=${ROOTFS:-$D/nh/distro/parrot}\n"
        "export R=${R:-$ROOTFS}\n"
        "export L=${L:-$D/usr/bin/elf_loader}\n"
        "export LX_LOG=${LX_LOG:-$HOME/.cache/lx}\n"
        "[ -d \"$ROOTFS/usr/lib/locale\" ] && export LOCPATH=\"$ROOTFS/usr/lib/locale\"\n"
        "export LC_ALL=${LC_ALL:-C.UTF-8}\n",
        stdout);
}

static void elf_print_init_zsh(void) {
    elf_print_init_env();
    fputs(
        "typeset -ga LX_PATH=(\n"
        "  $R/usr/local/sbin $R/usr/local/bin $R/usr/sbin $R/usr/bin $R/sbin $R/bin\n"
        "  $R/root/.nvm/versions/node/*/bin(N/On) $R/root/.local/bin\n"
        ")\n",
        stdout);
    fputs(LX_HELPERS_ZSH, stdout);
}

static void elf_print_init_bash(void) {
    elf_print_init_env();
    fputs(
        "LX_PATH=\"$R/usr/local/sbin $R/usr/local/bin $R/usr/sbin $R/usr/bin $R/sbin $R/bin\"\n"
        "for _nvm in $R/root/.nvm/versions/node/*/bin; do [ -d \"$_nvm\" ] && LX_PATH=\"$LX_PATH $_nvm\"; done\n"
        "[ -d \"$R/root/.local/bin\" ] && LX_PATH=\"$LX_PATH $R/root/.local/bin\"\n"
        "unset _nvm\n",
        stdout);
    fputs(LX_HELPERS_BASH, stdout);
}

int main(int argc, char **argv, char **envp) {
    /* ELF_DEBUG → unbuffered stdout, ať trace při SIGSEGV nekončí v bufferu */
    if (getenv("ELF_DEBUG"))
        setvbuf(stdout, NULL, _IONBF, 0);

    /* Ensure a valid RLIMIT_STACK (at least 8MB) so glibc NPTL pthread_create
     * does not fail in allocate_stack with size == 0 on Android host */
    struct rlimit rl_stack = { 8 * 1024 * 1024, 8 * 1024 * 1024 };
    syscall(SYS_prlimit64, 0, 3 /* RLIMIT_STACK */, &rl_stack, NULL);
    /* seccomp stacked filtr: nove syscalls (clone3/close_range/...) -> ENOSYS,
     * aby fungovaly glibc fallbacky pod app profilem jadra 4.14.
     * ELF_LOADER_NO_COMPAT=1 filtr preskoci (izolace, zda SIGSYS neni nas). */
    if (!getenv("ELF_LOADER_NO_COMPAT"))
        elf_install_compat();
    /* Fault handler hned od zacatku (driv jen s NO_COMPAT): jinak pady behem
     * nacitani modulu (pred elf_run) koncily tichym "Segmentation fault"
     * bez PC/adresy - napr. nahodny SIGSEGV ~5 % po "tls-done" libc.
     * elf_run ho pred skokem do guesta stejne reinstaluje. */
    elf_install_fault_handlers();
    /* The own-loaded parrot libc and the loader's host libc share the same
       process brk.  Both allocators must never shrink the heap (brk): a trim
       by either one unmaps live chunks of the other.  Set MALLOC_* tunables
       (read by parrot malloc at its first malloc) and mallopt the host.  */
    setenv("MALLOC_TRIM_THRESHOLD_", "2147483647", 0);
    setenv("MALLOC_MMAP_THRESHOLD_", "33554432", 0);
    setenv("MALLOC_TOP_PAD_", "8388608", 0);
    setenv("MALLOC_MMAP_MAX_", "1024", 0);
#ifdef __GLIBC__
    mallopt(M_TRIM_THRESHOLD, 0x7fffffff);
    mallopt(M_TOP_PAD, 8388608);
#endif

    /* guest-only LD_PRELOAD injection (ELF_LOADER_PRELOAD) — must happen
     * before dispatch so run()/run_ownall()/run_shim() see the patched envp */
    envp = elf_guest_envp(envp);

    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    int ai = 1;
    int lazy_was_set = 0;
    while (ai + 1 < argc && strcmp(argv[ai], "--lazy") == 0) {
        elf_set_lazy(1);
        lazy_was_set = 1;
        ai++;
    }
    if (lazy_was_set)
        if (elf_debug())
            fprintf(stderr, "[+] lazy PLT binding enabled\n");

    if (strcmp(argv[ai], "--help") == 0 || strcmp(argv[ai], "-h") == 0) {
        print_help(argv[0]);
        return 0;
    }

    if (strcmp(argv[ai], "--version") == 0 || strcmp(argv[ai], "-V") == 0) {
        puts("elf_loader " ELF_LOADER_VERSION "\n"
             "  own-loading glibc launcher for Android (aarch64)\n"
             "  (c) elf_loader project");
        return 0;
    }

    if (strcmp(argv[ai], "init") == 0) {
        const char *shell = (ai + 1 < argc) ? argv[ai + 1] : "zsh";
        if (strcmp(shell, "zsh") == 0) {
            elf_print_init_zsh();
            return 0;
        }
        if (strcmp(shell, "bash") == 0) {
            elf_print_init_bash();
            return 0;
        }
        fprintf(stderr, "elf_loader: init: nepodporovany shell '%s' (zsh|bash)\n", shell);
        return 1;
    }

    if (strcmp(argv[ai], "--check") == 0) {
        if (ai + 1 >= argc) {
            fprintf(stderr, "Usage: %s --check <file>\n", argv[0]);
            return 1;
        }
        const char *path = argv[ai + 1];
        elf_object_t *obj = elf_load(path);
        if (!obj) {
            fprintf(stderr, "elf_loader: %s: not a loadable ELF (missing, wrong arch, or corrupted)\n", path);
            return 2;
        }
        printf("elf_loader: %s: ELF%d %s, machine=%s, entry=%p, deps=%zu\n",
               path,
               obj->ehdr && (obj->ehdr->e_ident[EI_CLASS] == ELFCLASS64) ? 64 : 32,
               obj->ehdr ? (obj->ehdr->e_type == ET_EXEC ? "ET_EXEC" : obj->ehdr->e_type == ET_DYN ? "ET_DYN" : "ET_OTHER") : "unknown",
               obj->ehdr && obj->ehdr->e_machine == EM_AARCH64 ? "AArch64" : "unknown",
               obj->entry_point,
               obj->handle_count);
        elf_unload(obj);
        return 0;
    }

    if (strcmp(argv[ai], "--run") == 0) {
        if (ai + 1 >= argc) {
            fprintf(stderr, "Usage: %s --run <elf> [args..]\n", argv[0]);
            return 1;
        }
        return run(argv[ai + 1], argc - (ai + 1), &argv[ai + 1], envp);
    }

    if (strcmp(argv[ai], "--own") == 0) {
        if (ai + 2 >= argc) {
            fprintf(stderr, "Usage: %s --own <elf> <shared.so> [args..]\n",
                    argv[0]);
            return 1;
        }
        return run_own(argv[ai + 1], argv[ai + 2], argc - (ai + 3),
                       &argv[ai + 3], envp);
    }

    if (strcmp(argv[ai], "--ownall") == 0) {
        if (ai + 1 >= argc) {
            fprintf(stderr, "Usage: %s --ownall <elf> [args..]\n", argv[0]);
            return 1;
        }
        return run_ownall(argv[ai + 1], argc - (ai + 1), &argv[ai + 1], envp);
    }

    if (strcmp(argv[ai], "--shim") == 0) {
        if (ai + 1 >= argc) {
            fprintf(stderr, "Usage: %s --shim <elf> [args..]\n", argv[0]);
            return 1;
        }
        return run_shim(argv[ai + 1], argc - (ai + 1), &argv[ai + 1], envp);
    }

    introspect(argv[ai]);
    return 0;
}