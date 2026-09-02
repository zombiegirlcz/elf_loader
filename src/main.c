#define _GNU_SOURCE 1
#include "../include/elf_loader.h"

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
    const char *v = getenv("F2_FILTER");
    return v && v[0] && v[0] != '0';
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
static void *g_orig_stat = NULL, *g_orig_stat64 = NULL, *g_orig___xstat = NULL,
            *g_orig_lstat = NULL, *g_orig___lxstat = NULL;
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
typedef int (*fp_fileno_unlocked)(FILE *);
static fp_fileno_unlocked g_orig_fileno_unlocked = NULL;
typedef int (*fp_fileno)(FILE *);
static fp_fileno g_orig_fileno = NULL;
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
    return f ? f(loader_bin, na, envp) : -1;
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
    fp_dlopen ff = (fp_dlopen)g_orig_dlopen;
    return ff ? ff(path, f) : NULL;
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

static int shim_fileno_unlocked(FILE *fp) {
    if (elf_debug()) fprintf(stderr, "[dbg] fileno_unlocked called with fp=%p\n", fp);
    fp_fileno_unlocked f = (fp_fileno_unlocked)g_orig_fileno_unlocked;
    if (!f) {
        /* Fallback: use fileno if fileno_unlocked not found */
        fp_fileno f2 = (fp_fileno)g_orig_fileno;
        return f2 ? f2(fp) : -1;
    }
    return f(fp);
}

static int shim_fileno(FILE *fp) {
    if (elf_debug()) fprintf(stderr, "[dbg] fileno called with fp=%p\n", fp);
    fp_fileno f = (fp_fileno)g_orig_fileno;
    return f ? f(fp) : -1;
}

static f2_hook_t g_f2_hooks[] = {
    {"open",(void*)shim_open,&g_orig_open},{"open64",(void*)shim_open64,&g_orig_open64},
    {"__open",(void*)shim_open,&g_orig_open},{"__open64",(void*)shim_open64,&g_orig_open64},
    {"openat",(void*)shim_openat,&g_orig_openat},{"openat64",(void*)shim_openat64,&g_orig_openat64},
    {"__openat",(void*)shim_openat,&g_orig_openat},{"__openat64",(void*)shim_openat64,&g_orig_openat64},
    {"stat",(void*)shim_stat,&g_orig_stat},{"stat64",(void*)shim_stat64,&g_orig_stat64},
    {"__xstat",(void*)shim___xstat,&g_orig___xstat},{"lstat",(void*)shim_lstat,&g_orig_lstat},
    {"__lxstat",(void*)shim___lxstat,&g_orig___lxstat},
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
static void shim_register_overrides(void) {
    if (getenv("F2_DISABLE")) return;  /* debug: zadne override */
    g_shim_root = getenv("ROOTFS");
    g_shim_loader = getenv("ELF_LOADER");
    if (!g_shim_loader || !g_shim_loader[0]) g_shim_loader = "/proc/self/exe";
    for (size_t i = 0; i < sizeof g_f2_hooks / sizeof g_f2_hooks[0]; i++)
        if (f2_only_match(g_f2_hooks[i].n))
            elf_register_override(g_f2_hooks[i].n, g_f2_hooks[i].shim);
}
static void shim_install_hooks(void) {
    if (getenv("F2_DISABLE")) return;
    int ok = 0;
    for (size_t i = 0; i < sizeof g_f2_hooks / sizeof g_f2_hooks[0]; i++)
        if (f2_only_match(g_f2_hooks[i].n) && hook_install(&g_f2_hooks[i])) ok++;
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

    if (!g_exec_mode) g_exec_mode = "--ownall";
    g_shim_root = getenv("ROOTFS");
    g_shim_loader = getenv("ELF_LOADER");
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

    g_exe_base = (uintptr_t)obj->base_addr;
    ldso_install_exe_linkmap(obj, path);
    ldso_install_module_list(scope->mods, scope->count);
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

int main(int argc, char **argv, char **envp) {
    /* ELF_DEBUG → unbuffered stdout, ať trace při SIGSEGV nekončí v bufferu */
    if (getenv("ELF_DEBUG"))
        setvbuf(stdout, NULL, _IONBF, 0);

    /* Ensure a valid RLIMIT_STACK (at least 8MB) so glibc NPTL pthread_create
     * does not fail in allocate_stack with size == 0 on Android host */
    struct rlimit rl_stack = { 8 * 1024 * 1024, 8 * 1024 * 1024 };
    syscall(SYS_prlimit64, 0, 3 /* RLIMIT_STACK */, &rl_stack, NULL);
    /* seccomp stacked filtr: nove syscalls (clone3/close_range/...) -> ENOSYS,
     * aby fungovaly glibc fallbacky pod app profilem jadra 4.14 */
    elf_install_compat();
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

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <elf_binary>        (introspect)\n", argv[0]);
        fprintf(stderr, "       %s --run <elf> [args..] (execute)\n", argv[0]);
        fprintf(stderr, "       %s --lazy --run <elf> [args..]\n", argv[0]);
        fprintf(stderr, "       %s --own <elf> <shared.so> [args..]\n", argv[0]);
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