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
        "  %s <elf>                        (introspect)\n"
        "\n"
        "Modes:\n"
        "  --run         host-loader mode: execute ELF with host libc\n"
        "  --own         own-load one shared module into a private scope\n"
        "  --ownall      own-load all distro deps + guest binary (parrot glibc)\n"
        "  --shim        F2 path-translation shim for chroot-less guest paths\n"
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
        prog, prog, prog, prog, prog, prog, prog, prog,
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
                              (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt",
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
            *g_orig_rename = NULL, *g_orig_unlink = NULL, *g_orig_mkdir = NULL,
            *g_orig_mkdirat = NULL, *g_orig_rmdir = NULL;
static void *g_orig_execve = NULL, *g_orig_execv = NULL, *g_orig_execvp = NULL,
            *g_orig_execvpe = NULL, *g_orig_execveat = NULL;
static void *g_orig_fopen = NULL, *g_orig_fopen64 = NULL,
            *g_orig___xstat64 = NULL, *g_orig___lxstat64 = NULL, *g_orig___fxstatat64 = NULL,
            *g_orig_faccessat2 = NULL,
            *g_orig_getrlimit = NULL, *g_orig_prlimit64 = NULL;
static void *g_orig_fileno_unlocked = NULL;
static void *g_orig_fileno = NULL;
static void *g_orig_flockfile = NULL;
static void *g_orig_close = NULL;
static void *g_orig_setfsuid = NULL, *g_orig_setfsgid = NULL;
static void *g_orig_mprotect = NULL;
static void *g_orig_opendir = NULL, *g_orig_readlink = NULL, *g_orig_readlinkat = NULL,
            *g_orig_realpath = NULL, *g_orig_dlopen = NULL, *g_orig_chdir = NULL;

typedef struct f2_hook { const char *n; void *shim; void **orig; } f2_hook_t;

/* Najde volnou 4KB stranku do +-120MB od 'addr' (B range je +-128MB)
 * prohledanim mezer v /proc/self/maps. mmap(NULL) by dal stranku GB daleko,
 * mimo dosah vetve. */
static void *alloc_near(void *addr) {
    uintptr_t want = (uintptr_t)addr;
    uintptr_t mina = want - 0x7800000;
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
typedef int (*fp_unlink)(const char *);
static int shim_unlink(const char *p) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_unlink f = (fp_unlink)g_orig_unlink; return f ? f(path) : -1;
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
        if (linkbuf[0] == '/') {
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
    if (!root || !root[0]) return envp;

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
    size_t rl = g_shim_root ? shim_strlen(g_shim_root) : 0;

    /* Path resolution — check ROOTFS prefix FIRST, before exclusion list.
     * ROOTFS lives under /data/... which is in the exclude list; without this
     * ordering, guest binaries referenced by device-absolute path would be
     * passed to the raw execve and die on missing PT_INTERP. */
    if (rl && shim_strncmp(p, g_shim_root, rl) == 0 && (p[rl] == '/' || p[rl] == 0)) {
        /* Already prefixed with $ROOTFS */
        shim_strcpy(resolved, sizeof(resolved), p);
    } else if (shim_excluded(p) || shim_strncmp(p, "/system", 7) == 0 ||
        shim_strncmp(p, "/vendor", 7) == 0 || shim_strncmp(p, "/apex", 5) == 0 ||
        shim_strncmp(p, "/product", 8) == 0 || shim_strncmp(p, "/odm", 4) == 0) {
        /* Excluded / host binaries -> real execve */
        fp_execve f = (fp_execve)g_orig_execve;
        return f ? f(p, argv, envp) : -1;
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
     * ale zaloguji se do white.log, aby bylo videt co chybi. */
    if (!wl_match(p)) {
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
static char *shim_realpath(const char *p, char *b) {
    char x[8192]; const char *path = p; if (shim_translate(p, x, sizeof x)) path = x;
    fp_realpath f = (fp_realpath)g_orig_realpath; return f ? f(path, b) : NULL;
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
    if (p[0] == '/') {
        /* Excluded host cesty (/system, /vendor, /apex, /proc, ...) se
         * NESMI prekladat pod ROOTFS a NESMI se re-execovat pres loader.
         * Musi se spustit primo (real posix_spawnp), jinak loader pokusi
         * nacist bionicky binarku jako guest glibc a spadne na libc.so.
         * (shim_execve to dela spravne, tady chybelo early return). */
        if (shim_excluded(p) || shim_strncmp(p, "/system", 7) == 0 ||
            shim_strncmp(p, "/vendor", 7) == 0 || shim_strncmp(p, "/apex", 5) == 0 ||
            shim_strncmp(p, "/product", 8) == 0 || shim_strncmp(p, "/odm", 4) == 0) {
            fp_posix_spawnp f = (fp_posix_spawnp)g_orig_posix_spawnp;
            return f ? f(pid, p, fa, at, argv, envp) : -1;
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
        if (rl) {
            shim_strcpy(resolved, sizeof resolved, g_shim_root);
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
static int shim___fxstatat64(int dfd, const char *p, void *st, int flags) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_fstatat f = (fp_fstatat)g_orig___fxstatat64; return f ? f(dfd, path, st, flags) : -1;
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
    if (elf_debug()) fprintf(stderr, "[dbg] fileno_unlocked called with fp=%p\n", fp);
    if (!fp) {
        errno = EBADF;
        return -1;
    }
    fp_fileno_unlocked f = (fp_fileno_unlocked)g_orig_fileno_unlocked;
    return f ? f(fp) : -1;
}

static int shim_fileno(FILE *fp) {
    if (elf_debug()) fprintf(stderr, "[dbg] fileno called with fp=%p\n", fp);
    if (!fp) {
        errno = EBADF;
        return -1;
    }
    fp_fileno f = (fp_fileno)g_orig_fileno;
    return f ? f(fp) : -1;
}

static void shim_flockfile(FILE *fp) {
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
static int shim_close(int fd) {
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
static int shim_setfsuid(unsigned int uid) { (void)uid; return (int)getuid(); }
static int shim_setfsgid(unsigned int gid) { (void)gid; return (int)getgid(); }

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
                          void *res, int rerr) {
    char b[200]; char *i = b;
    const char *p = "[MMAP] len=";
    while (*p) *i++ = *p++;
    shim_hex(&i, len, 10);
    p = " addr="; while (*p) *i++ = *p++;
    shim_hex(&i, (unsigned long)addr, 12);
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
    int dbg = (addr != NULL) || (len >= (1UL << 30));
    if (addr && (flags & (int)SHIM_MAP_FIXED_NOREPLACE)) {
        unsigned long a = (unsigned long)addr;
        if (!shim_addr_range_free(a, a + len)) {
            shim_guest_errno_set(17);         /* EEXIST */
            if (dbg) shim_mmap_log(len, addr, flags, (void *)-1, 17);
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
    if (dbg) shim_mmap_log(len, addr, flags, r, e);
    return r;
}
static void *shim_mmap(void *a, unsigned long l, int p, int fl, int fd, long o) {
    return shim_mmap_common(a, l, p, fl, fd, o);
}
static void *shim_mmap64(void *a, unsigned long l, int p, int fl, int fd, long o) {
    return shim_mmap_common(a, l, p, fl, fd, o);
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
    {"newfstatat",(void*)shim_newfstatat,&g_orig_newfstatat},{"__fxstatat",(void*)shim_fstatat,&g_orig_fstatat},
    {"symlink",(void*)shim_symlink,&g_orig_symlink},{"symlinkat",(void*)shim_symlinkat,&g_orig_symlinkat},
    {"link",(void*)shim_link,&g_orig_link},{"rename",(void*)shim_rename,&g_orig_rename},
    {"unlink",(void*)shim_unlink,&g_orig_unlink},{"mkdir",(void*)shim_mkdir,&g_orig_mkdir},
    {"mkdirat",(void*)shim_mkdirat,&g_orig_mkdirat},{"rmdir",(void*)shim_rmdir,&g_orig_rmdir},
    {"execve",(void*)shim_execve,&g_orig_execve},{"execv",(void*)shim_execv,&g_orig_execv},
    {"execvp",(void*)shim_execvp,&g_orig_execvp},{"execvpe",(void*)shim_execvpe,&g_orig_execvpe},
    {"execveat",(void*)shim_execveat,&g_orig_execveat},
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
    {"prlimit64",(void*)shim_getrlimit,&g_orig_prlimit64},
    {"fileno_unlocked",(void*)shim_fileno_unlocked,&g_orig_fileno_unlocked},{"fileno",(void*)shim_fileno,&g_orig_fileno},
    {"flockfile",(void*)shim_flockfile,&g_orig_flockfile},
    {"close",(void*)shim_close,&g_orig_close},
    {"setfsuid",(void*)shim_setfsuid,&g_orig_setfsuid},
    {"setfsgid",(void*)shim_setfsgid,&g_orig_setfsgid},
    {"mprotect",(void*)shim_mprotect,&g_orig_mprotect},
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
        "/etc/nsswitch.conf", "/etc/hosts", NULL
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
    elf_register_override("pthread_getattr_np", (void *)shim_pthread_getattr_np);
    /* mmap/mmap64: emulace MAP_FIXED_NOREPLACE (kernel 4.14 ho nezna).
     * V8/Node s nim rezervuje 4GB pointer-compression cage na presne adrese;
     * bez emulace dostane jinou adresu -> divoke komprimovane pointery. */
    elf_register_override("mmap", (void *)shim_mmap);
    elf_register_override("mmap64", (void *)shim_mmap64);
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
    shim_install_hooks();    /* patch glibc leaf funkci (F2 / re-exec) */
    shim_resolve_fallback(); /* fallback real funkci (W^X) */

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
    else
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