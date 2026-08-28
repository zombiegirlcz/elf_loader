#define _GNU_SOURCE 1
#include "../include/elf_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <malloc.h>
#include <unistd.h>
#include <stdarg.h>
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

/* F2: nainstalujeme seccomp filtr, ktery pro Androidem blokovane syscally
 * vraci -errno misto SIGSYS (RET_TRAP). Filtr se dedi pres execve do glibc
 * procesu (signal handler naopak po execve zmizi, a glibc init vola
 * setfsuid jeste pred konstruktorem LD_PRELOAD .so -> handler by nezasahl).
 * RET_ERRNO ma vyssi prioritu nez appuv RET_TRAP, takze ho prebije.
 * setfsuid/setfsgid/setpriority/getpriority/mbind/... -> errno 0 (uspech),
 * keyctl/syslog/name_to_handle_at/faccessat2/mq/msg/sem/shm -> ENOSYS. */
static void install_f2_seccomp(void) {
    struct sock_filter f[96];
    int n = 0;
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          offsetof(struct seccomp_data, nr));
#define F2_ADD(nr, eno) do { \
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1); \
        f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, \
                  SECCOMP_RET_ERRNO | ((eno) & SECCOMP_RET_DATA)); \
    } while (0)
    /* best-effort: vrat 0 (uspech) */
    F2_ADD(151, 0);  /* setfsuid */
    F2_ADD(152, 0);  /* setfsgid */
    F2_ADD(140, 0);  /* setpriority */
    F2_ADD(141, 0);  /* getpriority */
    F2_ADD(235, 0);  /* mbind */
    F2_ADD(237, 0);  /* set_mempolicy */
    F2_ADD(238, 0);  /* migrate_pages */
    F2_ADD(239, 0);  /* move_pages */
    /* ENOSYS */
    F2_ADD(217, 38); /* add_key */
    F2_ADD(218, 38); /* request_key */
    F2_ADD(219, 38); /* keyctl */
    F2_ADD(236, 38); /* get_mempolicy */
    F2_ADD(116, 38); /* syslog */
    F2_ADD(264, 38); /* name_to_handle_at */
    F2_ADD(439, 38); /* faccessat2 */
    /* nove syscally, ktere glibc zkousi a pri ENOSYS fallbackne: */
    F2_ADD(435, 38); /* clone3 -> ENOSYS => glibc pouzije clone */
    F2_ADD(436, 38); /* close_range */
    F2_ADD(437, 38); /* openat2 */
    F2_ADD(434, 38); /* pidfd_open */
    F2_ADD(424, 38); /* pidfd_send_signal */
    F2_ADD(438, 38); /* pidfd_getfd */
    F2_ADD(444, 38); /* futex_waitv */
    F2_ADD(442, 38); /* landlock_create_ruleset */
    F2_ADD(443, 38); /* landlock_add_rule */
    F2_ADD(444, 38); /* futex_waitv */
    /* mq_* */
    F2_ADD(180, 38); F2_ADD(181, 38); F2_ADD(182, 38); F2_ADD(183, 38);
    F2_ADD(184, 38); F2_ADD(185, 38);
    /* msg* (SysV) */
    F2_ADD(186, 38); F2_ADD(187, 38); F2_ADD(188, 38); F2_ADD(189, 38);
    /* sem* (SysV) */
    F2_ADD(190, 38); F2_ADD(191, 38); F2_ADD(192, 38); F2_ADD(193, 38);
    /* shm* (SysV) */
    F2_ADD(194, 38); F2_ADD(195, 38); F2_ADD(198, 38); F2_ADD(199, 38);
#undef F2_ADD
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    struct sock_fprog p = { .len = (unsigned short)n, .filter = f };
    long pr = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    long sc = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &p);
    if (elf_debug()) {
        fprintf(stderr, "[F2-seccomp] prctl_nnp=%ld seccomp=%ld (n=%d)\n", pr, sc, n);
    }
}

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
static void *g_orig_access = NULL, *g_orig_faccessat = NULL;
static void *g_orig_statx = NULL, *g_orig_fstatat = NULL, *g_orig_newfstatat = NULL;
static void *g_orig_symlink = NULL, *g_orig_symlinkat = NULL, *g_orig_link = NULL,
            *g_orig_rename = NULL, *g_orig_unlink = NULL, *g_orig_mkdir = NULL,
            *g_orig_mkdirat = NULL, *g_orig_rmdir = NULL;
static void *g_orig_execve = NULL, *g_orig_execv = NULL, *g_orig_execvp = NULL;
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
static fp_open g_bionic_open = NULL;   /* zachytny bionic open (pred override) */
typedef int (*fp_openat)(int, const char *, int, ...);
static int shim_open(const char *p, int flags, ...) {
    char buf[8192]; const char *path = p;
    if (shim_translate(p, buf, sizeof buf)) path = buf;
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
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    fp_open f = (fp_open)g_orig_open64;
    return f ? f(path, flags, mode) : -1;
}
static int shim_openat(int dfd, const char *p, int flags, ...) {
    char buf[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, buf, sizeof buf)) path = buf; }
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    fp_openat f = (fp_openat)g_orig_openat;
    return f ? f(dfd, path, flags, mode) : -1;
}
static int shim_openat64(int dfd, const char *p, int flags, ...) {
    char buf[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, buf, sizeof buf)) path = buf; }
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
typedef int (*fp_faccessat)(int, const char *, int, int);
static int shim_stat(const char *p, struct stat *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_stat f = (fp_stat)g_orig_stat; return f ? f(path, st) : -1;
}
static int shim_stat64(const char *p, struct stat64 *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_stat64 f = (fp_stat64)g_orig_stat64; return f ? f(path, st) : -1;
}
static int shim___xstat(int v, const char *p, struct stat *st) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
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
    fp_access f = (fp_access)g_orig_access; return f ? f(path, m) : -1;
}
static int shim_faccessat(int dfd, const char *p, int m, int ff) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_faccessat f = (fp_faccessat)g_orig_faccessat; return f ? f(dfd, path, m, ff) : -1;
}

/* Moderni glibc/coreutils routuji stat/lstat/fstatat pres statx syscall.
 * Bez tohoto hooku zustane statx netranslatovany -> ENOENT na hostu. */
typedef int (*fp_statx)(int, const char *, int, unsigned int, void *);
static int shim_statx(int dfd, const char *p, int flags, unsigned int mask, void *stx) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_statx f = (fp_statx)g_orig_statx; return f ? f(dfd, path, flags, mask, stx) : -1;
}
typedef int (*fp_fstatat)(int, const char *, void *, int);
static int shim_fstatat(int dfd, const char *p, void *st, int flags) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
    fp_fstatat f = (fp_fstatat)g_orig_fstatat; return f ? f(dfd, path, st, flags) : -1;
}
static int shim_newfstatat(int dfd, const char *p, void *st, int flags) {
    char b[8192]; const char *path = p;
    if (dfd == -100 && p && p[0] == '/') { if (shim_translate(p, b, sizeof b)) path = b; }
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
static int shim_execve(const char *p, char *const argv[], char *const envp[]) {
    char b[8192]; const char *path = p; int tr = shim_translate(p, b, sizeof b);
    if (tr) path = b;
    if (tr) {
        char *na[256]; int n = 0;
        na[n++] = (char *)g_shim_loader; na[n++] = (char *)"--shim"; na[n++] = (char *)path;
        for (int i = 1; argv && argv[i] && n < 255; i++) na[n++] = argv[i];
        na[n] = NULL;
        fp_execve f = (fp_execve)g_orig_execve;
        return f ? f((char *)g_shim_loader, na, envp) : -1;
    }
    fp_execve f = (fp_execve)g_orig_execve;
    return f ? f(path, argv, envp) : -1;
}
static int shim_execv(const char *p, char *const argv[]) { return shim_execve(p, argv, environ); }
static int shim_execvp(const char *p, char *const argv[]) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_execvp f = (fp_execvp)g_orig_execvp; return f ? f(path, argv) : -1;
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
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_dlopen ff = (fp_dlopen)g_orig_dlopen; return ff ? ff(path, f) : NULL;
}
static int shim_chdir(const char *p) {
    char b[8192]; const char *path = p; if (shim_translate(p, b, sizeof b)) path = b;
    fp_chdir f = (fp_chdir)g_orig_chdir; return f ? f(path) : -1;
}

static f2_hook_t g_f2_hooks[] = {
    {"open",(void*)shim_open,&g_orig_open},{"open64",(void*)shim_open64,&g_orig_open64},
    {"__open",(void*)shim_open,&g_orig_open},{"__open64",(void*)shim_open64,&g_orig_open64},
    {"openat",(void*)shim_openat,&g_orig_openat},{"openat64",(void*)shim_openat64,&g_orig_openat64},
    {"__openat",(void*)shim_openat,&g_orig_openat},{"__openat64",(void*)shim_openat64,&g_orig_openat64},
    {"stat",(void*)shim_stat,&g_orig_stat},{"stat64",(void*)shim_stat64,&g_orig_stat64},
    {"__xstat",(void*)shim___xstat,&g_orig___xstat},{"lstat",(void*)shim_lstat,&g_orig_lstat},
    {"__lxstat",(void*)shim___lxstat,&g_orig___lxstat},
    {"access",(void*)shim_access,&g_orig_access},{"faccessat",(void*)shim_faccessat,&g_orig_faccessat},
    {"statx",(void*)shim_statx,&g_orig_statx},{"fstatat",(void*)shim_fstatat,&g_orig_fstatat},
    {"newfstatat",(void*)shim_newfstatat,&g_orig_newfstatat},{"__fxstatat",(void*)shim_fstatat,&g_orig_fstatat},
    {"symlink",(void*)shim_symlink,&g_orig_symlink},{"symlinkat",(void*)shim_symlinkat,&g_orig_symlinkat},
    {"link",(void*)shim_link,&g_orig_link},{"rename",(void*)shim_rename,&g_orig_rename},
    {"unlink",(void*)shim_unlink,&g_orig_unlink},{"mkdir",(void*)shim_mkdir,&g_orig_mkdir},
    {"mkdirat",(void*)shim_mkdirat,&g_orig_mkdirat},{"rmdir",(void*)shim_rmdir,&g_orig_rmdir},
    {"execve",(void*)shim_execve,&g_orig_execve},{"execv",(void*)shim_execv,&g_orig_execv},
    {"execvp",(void*)shim_execvp,&g_orig_execvp},{"opendir",(void*)shim_opendir,&g_orig_opendir},
    {"readlink",(void*)shim_readlink,&g_orig_readlink},{"readlinkat",(void*)shim_readlinkat,&g_orig_readlinkat},
    {"realpath",(void*)shim_realpath,&g_orig_realpath},{"dlopen",(void*)shim_dlopen,&g_orig_dlopen},
    {"chdir",(void*)shim_chdir,&g_orig_chdir},
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
static int g_f2_active = 0;  /* 1 = F2 rezim (--shim), povol inline-hooky */
static int run_shim(const char *path, int argc, char **argv, char **envp) {
    g_f2_active = 1;
    shim_register_overrides();
    if (elf_debug())
        fprintf(stderr, "[+] F2 path-translation shim (ROOTFS=%s)\n",
                g_shim_root ? g_shim_root : "(unset)");
    /* Override se registruji pred elf_load (shim_register_overrides v run_shim),
     * takze cat i libc se zrelokuji s prekladem. Glibc leaf funkce se patchuji
     * az po elf_load (shim_install_hooks nize), az je libc nactena.
     * pres zachytny bionic open, ne prekladany glibc open). */
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

    g_loader_active = 1;  /* loaderuv kod (bionic TLS): jeho open = bionicky */
    elf_object_t *obj = elf_load(path);
    elf_own_scope = NULL;
    elf_set_crash_scope(scope);
    if (!obj) {
        elf_scope_destroy(scope);
        return 1;
    }
    if (g_f2_active) shim_install_hooks();  /* patch glibc leaf funkci (F2) */
    if (g_f2_active) shim_resolve_fallback(); /* fallback real funkci (W^X) */

    void *libc_obj = NULL;
    for (size_t mi = 0; mi < scope->count; mi++)
        if (scope->mods[mi]->soname && strstr(scope->mods[mi]->soname, "libc.so.6"))
            libc_obj = scope->mods[mi]->base_addr;
    g_libc_base = (uintptr_t)libc_obj;

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