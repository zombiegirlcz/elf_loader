#define _GNU_SOURCE
#include "../include/elf_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
extern char **environ;
#include <signal.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <limits.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>

static size_t g_page_size = 0;
static size_t sys_page_size(void) {
    if (!g_page_size)
        g_page_size = (size_t)sysconf(_SC_PAGESIZE);
    return g_page_size;
}
#define PAGE_SIZE sys_page_size()
#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))

#define MAX_OVERRIDES 64

typedef struct {
    const char *name;
    void *fn;
} override_t;

static override_t overrides[MAX_OVERRIDES];
static size_t override_count = 0;

static int lazy_binding = 0;
static elf_object_t *lazy_current = NULL;
#define MAX_LAZY_OBJS 64
static elf_object_t *lazy_objs[MAX_LAZY_OBJS];
static size_t lazy_obj_count = 0;

int elf_own_deps = 0;
elf_scope_t *elf_own_scope = NULL;
/* Pro fault handler: scope načtených modulů, aby šel pc/lr mapovat na soname+off. */
static elf_scope_t *g_crash_scope = NULL;
void elf_set_crash_scope(elf_scope_t *s) { g_crash_scope = s; }

int elf_init_argc = 0;
char **elf_init_argv = NULL;
char **elf_init_envp = NULL;

const char *loader_phase = "start";
uintptr_t g_libc_base = 0;
uintptr_t g_exe_base = 0;

static int is_ld_linux(const char *name) {
    return strncmp(name, "ld-linux", 8) == 0 || strcmp(name, "ld.so.1") == 0;
}

#define SYSTEM_LIBDIRS "/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu" \
                       ":/usr/lib:/lib"

static char *derive_distro_libdirs(const char *origin_dir);

/* Proaktivně own-loadnout distro ld.so (ld-linux-aarch64.so.1) do scope.
 * GLIBC_PRIVATE symboly (_dl_exception_create, _dl_signal_error, …) a
 * u glibc < 2.30 i __tls_get_addr žijí v ld.so; libc je importuje → musí
 * být ve scope už při jejím loadu. Starší rootfs (glibc 2.28, Termux
 * proot-distro) bez toho padají na unresolved JUMP_SLOTs → SIGSEGV. */
static void preload_distro_ldso(const char *osearch, elf_scope_t *scope);

static const char *sys_libdirs(void) {
    static char buf[512];
    static int done = 0;
    if (!done) {
        const char *root = getenv("ELF_ROOTFS");
        if (root && root[0]) {
            snprintf(buf, sizeof buf, "%s/usr/lib/aarch64-linux-gnu:%s/lib/aarch64-linux-gnu:%s/usr/lib:%s/lib",
                     root, root, root, root);
        } else {
            snprintf(buf, sizeof buf, "%s", SYSTEM_LIBDIRS);
        }
        done = 1;
    }
    return buf;
}

/* Verbose loader logging: default TICHY (cisty vystup spustene binarky).
 * ELF_DEBUG=<cokoli krome "0"> zapne [+] / [dbg] trace zpet na stderr.
 * getenv nealokuje -> bezpecne i pro malloc-free resolve path. */
int elf_debug(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *d = getenv("ELF_DEBUG");
        cached = (d && d[0] && strcmp(d, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static size_t map_base_vaddr(const elf_object_t *obj);
static void *va(const elf_object_t *obj, size_t vaddr);
static void apply_segment_prots(elf_object_t *obj);
static void maybe_fixup_bionic_phdr(elf_object_t *obj);
static char *find_in_paths(const char *soname, const char *search);

void elf_set_lazy(int on) {
    lazy_binding = on;
}

extern void lazy_plt_stub(void);

struct ifunc_arg_t {
    unsigned long size;
    unsigned long hwcap;
    unsigned long hwcap2;
    Elf64_auxv_t auxv[2];
};

static void *call_ifunc_resolver(void *resolver) {
    struct ifunc_arg_t args;
    args.size = sizeof(args);
    args.hwcap = getauxval(AT_HWCAP);
    args.hwcap2 = getauxval(AT_HWCAP2);
    args.auxv[0].a_type = AT_NULL;
    args.auxv[0].a_un.a_val = 0;
    args.auxv[1].a_type = AT_NULL;
    args.auxv[1].a_un.a_val = 0;
    return ((void *(*)(struct ifunc_arg_t *))resolver)(&args);
}

/* ---- emulation of glibc's ld.so-private runtime state ---- */

#define LDSO_RO_SIZE 0x400
#define LDSO_GLOBAL_SIZE 0xc00

static unsigned char ldso_ro[LDSO_RO_SIZE];
static unsigned char ldso_global[LDSO_GLOBAL_SIZE];
static unsigned int ldso_enable_secure;
static uintptr_t ldso_pointer_chk_guard;

/* Statická proměnná jako __stack_chk_guard pro Parrot glibc (je UND v libc.so.6,
 * musí ji dodat ld.so; GLOB_DAT ukládá do GOT slotu ADRESU této proměnné).  */
static uint64_t ldso_stack_guard = 0xdeadbeefcafe1234ULL;
static int64_t ldso_rseq_offset;
static unsigned int ldso_rseq_size;
static void *ldso_stack_end;
static char ldso_platform[] = "aarch64";
static char *ldso_argv_copy[8];
static Elf64_auxv_t ldso_auxv[64];
static int ldso_setup_done;

/* Minimal glibc-shaped struct link_map for the main executable.  glibc reads
   GL(dl_ns)[0]._ns_loaded (offset 0 of _rtld_global) and then l->l_info[DT_INIT]
   (+0xa0) / l_info[DT_INIT_ARRAY] (+0x108) to run the exe's constructors; we
   leave those NULL because the loader already ran them.  dl_iterate_phdr /
   _dl_find_object additionally read l_real(+0x28), l_phdr(+0x2f0),
   l_phnum(+0x300), l_contiguous(0x366 bit3), l_map_start(+0x398),
   l_map_end(+0x3a0), l_tls_modid(+0x498).  */
static uint64_t ldso_exe_linkmap[0x100];
static char ldso_exe_name[256];

void ldso_install_exe_linkmap(elf_object_t *exe, const char *name) {
    memset(ldso_exe_linkmap, 0, sizeof ldso_exe_linkmap);
    unsigned char *lm = (unsigned char *)ldso_exe_linkmap;
    uintptr_t base = (uintptr_t)exe->base_addr;
    *(uintptr_t *)(lm + 0x00) = base;                       /* l_addr */
    if (name) {
        strncpy(ldso_exe_name, name, sizeof ldso_exe_name - 1);
        ldso_exe_name[sizeof ldso_exe_name - 1] = 0;
    }
    *(uintptr_t *)(lm + 0x08) = (uintptr_t)ldso_exe_name;   /* l_name */
    *(uintptr_t *)(lm + 0x28) = (uintptr_t)ldso_exe_linkmap;/* l_real = self */
    *(uintptr_t *)(lm + 0x2f0) = (uintptr_t)exe->phdr;      /* l_phdr */
    *(uint16_t *)(lm + 0x300) = (uint16_t)exe->phdr_count;  /* l_phnum */
    lm[0x366] = 0x8;                                        /* l_contiguous */
    *(uintptr_t *)(lm + 0x398) = base;                      /* l_map_start */
    *(uintptr_t *)(lm + 0x3a0) = base + exe->total_size;    /* l_map_end */
    /* l_tls_modid (+0x498) left 0: loader resolves TLS directly */
    ((uint64_t *)ldso_global)[0] = (uintptr_t)ldso_exe_linkmap;
}

/* Per-module fake link_map structs so _dl_find_dso_for_object /
   _dl_find_object / dl_iterate_phdr can attribute addresses to objects.  The
   layout mirrors ldso_exe_linkmap.  */
#define LDSO_MAX_MODULES 64
static uint64_t ldso_module_linkmaps[LDSO_MAX_MODULES][0x100];
static char ldso_module_names[LDSO_MAX_MODULES][256];
static size_t ldso_module_count;
static int ldso_modules_built;

void ldso_install_module_list(elf_object_t *const *mods, size_t count) {
    if (count > LDSO_MAX_MODULES)
        count = LDSO_MAX_MODULES;
    for (size_t i = 0; i < count; i++) {
        elf_object_t *m = mods[i];
        if (!m)
            continue;
        uint64_t *lm = ldso_module_linkmaps[ldso_module_count];
        memset(lm, 0, sizeof ldso_module_linkmaps[0]);
        unsigned char *b = (unsigned char *)lm;
        uintptr_t base = (uintptr_t)m->base_addr;
        const char *name = m->soname ? m->soname : "";
        strncpy(ldso_module_names[ldso_module_count], name,
                sizeof ldso_module_names[0] - 1);
        ldso_module_names[ldso_module_count][sizeof ldso_module_names[0] - 1] = 0;
        *(uintptr_t *)(b + 0x00) = base;                          /* l_addr */
        *(uintptr_t *)(b + 0x08) = (uintptr_t)ldso_module_names[ldso_module_count];
        *(uintptr_t *)(b + 0x28) = (uintptr_t)lm;                 /* l_real */
        *(uintptr_t *)(b + 0x2f0) = (uintptr_t)m->phdr;           /* l_phdr */
        *(uint16_t *)(b + 0x300) = (uint16_t)m->phdr_count;       /* l_phnum */
        b[0x366] = 0x8;                                           /* l_contiguous */
        *(uintptr_t *)(b + 0x398) = base;                         /* l_map_start */
        *(uintptr_t *)(b + 0x3a0) = base + m->total_size;         /* l_map_end */
        ldso_module_count++;
    }
    ldso_modules_built = 1;
}

/* New-glibc _dl_find_dso_for_object: locate the object whose mapped range
   contains ADDR and return its struct link_map *, or NULL.  The extra
   arguments are cache/bookkeeping hints the caller passes; they are
   irrelevant for a correct linear lookup.  */
static void *ldso_find_dso_for_object(uintptr_t addr, long a1, long a2,
                                      long a3, int a4, long a5, long a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!addr)
        return NULL;
    {
        uintptr_t start = *(uintptr_t *)((unsigned char *)ldso_exe_linkmap + 0x398);
        uintptr_t end = *(uintptr_t *)((unsigned char *)ldso_exe_linkmap + 0x3a0);
        if (addr >= start && addr < end)
            return ldso_exe_linkmap;
    }
    for (size_t i = 0; i < ldso_module_count; i++) {
        unsigned char *b = (unsigned char *)ldso_module_linkmaps[i];
        uintptr_t start = *(uintptr_t *)(b + 0x398);
        uintptr_t end = *(uintptr_t *)(b + 0x3a0);
        if (addr >= start && addr < end)
            return ldso_module_linkmaps[i];
    }
    return NULL;
}

/* glibc 2.41+ _dl_find_object: (const void *pc, struct dl_find_object *result).
   Fills result for the object containing PC, returns 0 on success / -1 on
   not-found.  Result layout: +0 dlfo_addr, +8 dlfo_name, +16 dlfo_phdr,
   +24 dlfo_phnum, +32 dlfo_map_start, +40 dlfo_map_end, +48 dlfo_link_map.  */
static int ldso_find_object(uintptr_t pc, void *result) {
    uintptr_t *dlfo = (uintptr_t *)result;
    void *lm = ldso_find_dso_for_object(pc, 0, 0, 0, 0, 0, 0);
    if (!dlfo || !lm) {
        if (dlfo)
            dlfo[0] = 0;
        return -1;
    }
    unsigned char *b = (unsigned char *)lm;
    dlfo[0] = *(uintptr_t *)(b + 0x00);   /* dlfo_addr = l_addr */
    dlfo[1] = *(uintptr_t *)(b + 0x08);   /* dlfo_name */
    dlfo[2] = *(uintptr_t *)(b + 0x2f0);  /* dlfo_phdr */
    *(uint16_t *)((unsigned char *)dlfo + 24) = *(uint16_t *)(b + 0x300);
    dlfo[4] = *(uintptr_t *)(b + 0x398);  /* dlfo_map_start */
    dlfo[5] = *(uintptr_t *)(b + 0x3a0);  /* dlfo_map_end */
    dlfo[6] = (uintptr_t)lm;              /* dlfo_link_map */
    return 0;
}

/* glibc 2.41+ _dl_catch_exception: runs operate(args) under exception
   protection.  Returns 0 on success and reports exception state through the
   out-params (NULL / 1 on success).  Our operate closures never raise, so the
   happy path always applies.  */
static int ldso_catch_exception(void *exc, void **result,
                                unsigned char *cancelled,
                                void (*operate)(void *), void *args) {
    (void)exc;
    *result = NULL;
    *cancelled = 1;
    operate(args);
    return 0;
}

static void ldso_debug_state(void) {
}

/* dl_tls_get_addr_soft: current thread's TLS block for a module, or NULL.  */
static void *ldso_tls_get_addr_soft(void *l) {
    (void)l;
    return NULL;
}

static void ldso_noop(void) {
}

static void ldso_setup(void) {
    if (ldso_setup_done)
        return;
    ldso_setup_done = 1;

    memset(ldso_ro, 0, sizeof ldso_ro);
    memset(ldso_global, 0, sizeof ldso_global);
    memset(ldso_argv_copy, 0, sizeof ldso_argv_copy);

    if (elf_init_argv) {
        for (int i = 0; i < 7 && elf_init_argv[i]; i++)
            ldso_argv_copy[i] = elf_init_argv[i];
    }

    memset(ldso_auxv, 0, sizeof ldso_auxv);
    int n = 0;
    if (elf_init_envp) {
        char **e = elf_init_envp;
        while (*e)
            e++;
        Elf64_auxv_t *av = (Elf64_auxv_t *)(e + 1);
        for (; n < 62 && av[n].a_type != AT_NULL; n++)
            ldso_auxv[n] = av[n];
        ldso_auxv[n].a_type = AT_NULL;
        ldso_auxv[n].a_un.a_val = 0;
    }

    uint64_t *ro = (uint64_t *)ldso_ro;
    ro[1]  = (uintptr_t)ldso_platform;           /* +0x08 _dl_platform */
    ro[2]  = strlen(ldso_platform);              /* +0x10 _dl_platformlen */
    ro[3]  = PAGE_SIZE;                          /* +0x18 _dl_pagesize */
    ro[4]  = 0x1400;                             /* +0x20 _dl_minsigstacksize */
    ro[8]  = 100;                                /* +0x40 _dl_clktck */
    ro[12] = getauxval(AT_HWCAP);                /* +0x60 _dl_hwcap */
    ro[13] = (uintptr_t)ldso_auxv;               /* +0x68 _dl_auxv */
    /* +0x70 midr_el1: 0 -> generic ifunc variants */
    /* glibc function-pointer slots called through _rtld_global_ro */
    ro[0x270 / 8] = (uintptr_t)ldso_find_dso_for_object;  /* _dl_find_dso_for_object */
    ro[0x280 / 8] = (uintptr_t)ldso_catch_exception;  /* _dl_catch_exception */
    ro[0x288 / 8] = (uintptr_t)ldso_debug_state;      /* _dl_debug_state */
    ro[0x290 / 8] = (uintptr_t)ldso_tls_get_addr_soft;/* dl_tls_get_addr_soft */
    ro[0x2a0 / 8] = (uintptr_t)ldso_find_object;      /* _dl_find_object */

    uint64_t *g = (uint64_t *)ldso_global;
    g[0xa80 / 8] = 1;                           /* _dl_nns */
    g[0xb18 / 8] = 1;                           /* dl_load_adds */
    /* dl_load_write_lock at +0xab8 stays all-zero (unlocked initial state) */
}

#define PARROT_HEAP_SIZE 0x4000000u  /* 64 MB */
static void *parrot_heap_base;
static char *parrot_brk_cur;

static void ldso_private_heap_init(void) {
    if (parrot_heap_base)
        return;
    void *base = mmap((void *)0x7f00000000UL, PARROT_HEAP_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (base == MAP_FAILED)
        base = mmap(NULL, PARROT_HEAP_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        base = NULL;
    parrot_heap_base = base;
    parrot_brk_cur = (char *)base;
}

void *ldso_sbrk(long inc) {
    if (!parrot_heap_base)
        ldso_private_heap_init();
    if (!parrot_heap_base)
        return (void *)-1;
    char *old = parrot_brk_cur;
    char *lo = (char *)parrot_heap_base;
    char *hi = lo + PARROT_HEAP_SIZE;
    char *nxt = old + inc;
    if (nxt < lo)
        nxt = lo;
    if (nxt > hi)
        nxt = hi;
    parrot_brk_cur = nxt;
    return old;
}

int ldso_brk(void *addr) {
    if (!parrot_heap_base)
        ldso_private_heap_init();
    if (!parrot_heap_base)
        return -1;
    char *lo = (char *)parrot_heap_base;
    char *hi = lo + PARROT_HEAP_SIZE;
    if ((char *)addr < lo || (char *)addr > hi)
        return -1;
    parrot_brk_cur = (char *)addr;
    return 0;
}

void *ldso_parrot_heap_base(void) {
    ldso_private_heap_init();
    return parrot_heap_base;
}

/* Tvrdý konec při chybějící kritické závislosti: bez ní by zůstaly
 * unresolved GOT sloty (=0) a program padl SIGSEGV pc=0x0 mnohem později
 * na nesouvisejícím místě. Lepší čistá chyba hned teď. */
static int is_core_lib(const char *soname) {
    static const char *core[] = {
        "libc.so.6", "libm.so.6", "libpthread.so.0",
        "libdl.so.2", "librt.so.1", "ld-linux-aarch64.so.1"
    };
    for (size_t i = 0; i < sizeof core / sizeof core[0]; i++)
        if (strcmp(soname, core[i]) == 0) return 1;
    return 0;
}

static void fatal_missing_dep(const char *soname, const char *searched) {
    fprintf(stderr,
            "\n[-] FATAL: required dependency \"%s\" was not found\n"
            "    searched paths: %s\n"
            "    hint: check LD_LIBRARY_PATH / ELF_ROOTFS point to the distro\n"
            "    hint: absolute symlinks in rootfs must be relative (chroot-less run)\n\n",
            soname, searched ? searched : "(none)");
    exit(1);
}

static void write_heap_veneer(void *target, void *fn) {
    uint32_t *p = (uint32_t *)target;
    uintptr_t page = (uintptr_t)target & ~0xfffUL;
    mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    p[0] = 0x58000050u;  /* ldr x16, [pc, #8] */
    p[1] = 0xd61f0200u;  /* br x16 */
    memcpy(&p[2], &fn, 8);
    __builtin___clear_cache(target, (char *)target + 16);
    mprotect((void *)page, 0x2000, PROT_READ | PROT_EXEC);
}

/* Android app seccomp filtr killne nove syscalls (clone3/close_range/
 * openat2/faccessat2) TRAPem — viz elf_install_compat.
 * Veneery sbrk/brk zachovavaji private heap pro parrot libc. */
#include <errno.h>
static void sys_write(int fd, const void *buf, size_t n);

/* Veneer fork -> raw clone(SIGCHLD). App sandbox: clone3 TRAP (SIGSYS),
 * fork-style clone EPERM pro app uid; root kontext clone projde.
 * Bez SETTID/CLEARTID: child_tidptr by musel ukazovat do parrot TLS tid
 * slotu — pro exec-and-go deti (sh/bash pipeline) nepovinne. */
static pid_t ldso_fork(void) {
    return (pid_t)syscall((long)220 /* __NR_clone */, (unsigned long)SIGCHLD,
                          0UL, 0UL, 0UL, 0UL);
}

static void patch_module_heap_syms(elf_object_t *m) {
    if (!m || !m->dynsym || !m->dynstr)
        return;
    uintptr_t mbv = 0;
    for (int i = 0; i < m->phdr_count; i++) {
        if (m->phdr[i].p_type == PT_LOAD) {
            mbv = m->phdr[i].p_vaddr - m->phdr[i].p_offset;
            break;
        }
    }
    for (size_t j = 0; j < m->dynsym_count; j++) {
        const Elf64_Sym *sym = &m->dynsym[j];
        if (sym->st_shndx == SHN_UNDEF)
            continue;
        const char *nm = m->dynstr + sym->st_name;
        void *fn = NULL;
        if (strcmp(nm, "sbrk") == 0 || strcmp(nm, "__sbrk") == 0)
            fn = (void *)ldso_sbrk;
        else if (strcmp(nm, "brk") == 0 || strcmp(nm, "__brk") == 0)
            fn = (void *)ldso_brk;
        else if (strcmp(nm, "fork") == 0 || strcmp(nm, "__fork") == 0)
            fn = (void *)ldso_fork;
        if (fn) {
            void *tgt = (char *)m->base_addr + (sym->st_value - mbv);
            write_heap_veneer(tgt, fn);
        }
    }
}

typedef struct { int64_t numval; } ldso_tunable_val_t;

typedef struct {
    const char *env_name;
    int64_t default_val;
    int is_size_t;
} malloc_tunable_t;

static const malloc_tunable_t malloc_tunables[] = {
    [0]  = { "MALLOC_CHECK_",          0,           0 },
    [1]  = { "MALLOC_TOP_PAD_",        131072,      1 },
    [2]  = { "MALLOC_PERTURB_",        0,           0 },
    [3]  = { "MALLOC_MMAP_THRESHOLD_", 128 * 1024,  1 },
    [4]  = { "MALLOC_TRIM_THRESHOLD_", 128 * 1024,  1 },
    [5]  = { "MALLOC_MMAP_MAX_",       65536,       0 },
    [6]  = { "MALLOC_ARENA_TEST",      0,           1 },
    [7]  = { "MALLOC_ARENA_MAX",       0,           1 },
    [8]  = { "MALLOC_MXFAST_",         128 * 1024,  1 },
    [9]  = { "MALLOC_TCACHE_COUNT",    7,           1 },
    [10] = { "MALLOC_TCACHE_MAX",      0,           1 },
    [11] = { "MALLOC_TCACHE_UNSORTED_LIMIT", 0,     1 },
    [12] = { "MALLOC_MMAP_THRESHOLD_DYNAMIC", 1,   0 },
    [13] = { "MALLOC_TRIM_THRESHOLD_DYNAMIC", 1,   0 },
    [14] = { "MALLOC_HUGETLB",         0,           0 },
};

static int tunable_is_initialized(int id) {
    if (id < 10)
        return 0;
    int idx = id - 10;
    if (idx >= 0 && idx < (int)(sizeof(malloc_tunables) / sizeof(malloc_tunables[0]))) {
        const char *env = getenv(malloc_tunables[idx].env_name);
        return env && env[0] != '\0';
    }
    return 0;
}

static void tunable_get_default(int id, void *valp) {
    if (id >= 10) {
        int idx = id - 10;
        if (idx >= 0 && idx < (int)(sizeof(malloc_tunables) / sizeof(malloc_tunables[0]))) {
            if (malloc_tunables[idx].is_size_t)
                *(int64_t *)valp = malloc_tunables[idx].default_val;
            else
                *(int32_t *)valp = (int32_t)malloc_tunables[idx].default_val;
            return;
        }
    }
    *(int64_t *)valp = 0;
}

/* Musí být plain static: __thread by NDK clang zkompiloval jako emulated TLS
 * (__emutls_get_address -> bionic pthread_once/pthread_getspecific), které
 * pod parrot TPIDR_EL0 (po switch_tls v jump_to_entry) padají — parrot glibc
 * volá __tunable_get_val až za entry (malloc/locale init). Loader je zde
 * single-threaded, prostá statická proměnná bohatě stačí. */
static int tunable_recursion_guard = 0;

static void tunable_get_val(int id, void *valp, void (*cb)(void *)) {
    if (tunable_recursion_guard) {
        *(int64_t *)valp = 0;
        return;
    }
    tunable_recursion_guard = 1;

    if (id >= 10) {
        int idx = id - 10;
        if (idx >= 0 && idx < (int)(sizeof(malloc_tunables) / sizeof(malloc_tunables[0]))) {
            int64_t v;
            const char *env = getenv(malloc_tunables[idx].env_name);
            if (env && env[0] != '\0')
                v = strtoll(env, NULL, 0);
            else
                v = malloc_tunables[idx].default_val;
            if (malloc_tunables[idx].is_size_t)
                *(int64_t *)valp = v;
            else
                *(int32_t *)valp = (int32_t)v;
            if (cb) {
                ldso_tunable_val_t cbv = { .numval = (uint64_t)v };
                cb(&cbv);
            }
            tunable_recursion_guard = 0;
            return;
        }
    }
    *(int64_t *)valp = 0;
    if (cb) {
        ldso_tunable_val_t v = { .numval = 0 };
        cb(&v);
    }
    tunable_recursion_guard = 0;
}

static void ldso_signal_error(void) {
    abort();
}

static void *ldso_lookup(const char *name) {
    if (!name)
        return NULL;
    if (strcmp(name, "_rtld_global_ro") == 0)
        return ldso_ro;
    if (strcmp(name, "_rtld_global") == 0)
        return ldso_global;
    if (strcmp(name, "_dl_argv") == 0)
        return ldso_argv_copy;
    if (strcmp(name, "__libc_enable_secure") == 0)
        return &ldso_enable_secure;
    if (strcmp(name, "__pointer_chk_guard") == 0)
        return &ldso_pointer_chk_guard;
    if (strcmp(name, "__stack_chk_guard") == 0)
        return &ldso_stack_guard;
    if (strcmp(name, "__rseq_offset") == 0)
        return &ldso_rseq_offset;
    if (strcmp(name, "__rseq_size") == 0)
        return &ldso_rseq_size;
    if (strcmp(name, "__libc_stack_end") == 0)
        return &ldso_stack_end;
    if (strcmp(name, "__tunable_get_val") == 0)
        return (void *)tunable_get_val;
    if (strcmp(name, "__tunable_get_default") == 0)
        return (void *)tunable_get_default;
    if (strcmp(name, "__tunable_is_initialized") == 0)
        return (void *)tunable_is_initialized;
    if (strcmp(name, "_dl_signal_error") == 0 ||
        strcmp(name, "_dl_signal_exception") == 0 ||
        strcmp(name, "_dl_catch_exception") == 0) {
        if (strcmp(name, "_dl_catch_exception") == 0)
            return (void *)ldso_catch_exception;
        return (void *)ldso_signal_error;
    }
    if (strcmp(name, "_dl_allocate_tls") == 0 ||
        strcmp(name, "_dl_allocate_tls_init") == 0 ||
        strcmp(name, "_dl_deallocate_tls") == 0 ||
        strcmp(name, "_dl_find_dso_for_object") == 0) {
        if (strcmp(name, "_dl_find_dso_for_object") == 0)
            return (void *)ldso_find_dso_for_object;
        return (void *)ldso_signal_error;
    }
    if (strcmp(name, "_dl_find_object") == 0)
        return (void *)ldso_find_object;
    if (strcmp(name, "_dl_rtld_di_serinfo") == 0)
        return (void *)ldso_signal_error;
    if (strcmp(name, "_dl_audit_preinit") == 0 ||
        strcmp(name, "_dl_audit_symbind_alt") == 0)
        return (void *)ldso_noop;
    if (strcmp(name, "brk") == 0 || strcmp(name, "__brk") == 0)
        return (void *)ldso_brk;
    if (strcmp(name, "sbrk") == 0 || strcmp(name, "__sbrk") == 0)
        return (void *)ldso_sbrk;
    return NULL;
}

static void *resolve_import_ldso(const char *name) {
    ldso_setup();
    return ldso_lookup(name);
}

static void *resolve_jmp_symbol(elf_object_t *obj, Elf64_Rela *r) {
    void *addr = NULL;
    size_t sym_idx = ELF64_R_SYM(r->r_info);
    if (sym_idx < obj->dynsym_count) {
        const Elf64_Sym *s = &obj->dynsym[sym_idx];
        const char *name = obj->dynstr + s->st_name;
        int is_ifunc = 0;
        if (s->st_shndx == SHN_UNDEF) {
            if (obj && obj->scope) {
                const Elf64_Sym *fs = NULL;
                elf_object_t *m = elf_scope_find(obj->scope, name, &fs);
                if (m && fs) {
                    if (ELF64_ST_TYPE(fs->st_info) == STT_GNU_IFUNC)
                        is_ifunc = 1;
                    addr = (char *)m->base_addr + (fs->st_value - map_base_vaddr(m));
                }
            }
            if (!addr)
                addr = elf_resolve_import(obj, name);
        } else {
            addr = va(obj, s->st_value);
        }
        if (addr && is_ifunc)
            addr = call_ifunc_resolver(addr);
    }
    return addr;
}

void *elf_lazy_resolve(uintptr_t got_slot) {
    elf_object_t *obj = NULL;
    for (size_t k = 0; k < lazy_obj_count; k++) {
        elf_object_t *cand = lazy_objs[k];
        if (!cand)
            continue;
        uintptr_t lo = (uintptr_t)cand->base_addr;
        uintptr_t hi = lo + cand->total_size;
        if (got_slot >= lo && got_slot < hi) {
            obj = cand;
            break;
        }
    }
    if (!obj)
        obj = lazy_current;
    if (!obj) {
        if (elf_debug())
            fprintf(stderr, "[!] lazy: no obj for slot %p\n", (void *)got_slot);
        return 0;
    }
    size_t mbv = map_base_vaddr(obj);
    for (size_t off = 0; obj->jmp_rela && off < obj->jmp_size;
         off += sizeof(Elf64_Rela)) {
        Elf64_Rela *r = (Elf64_Rela *)((char *)obj->jmp_rela + off);
        if ((uintptr_t)((char *)obj->base_addr + (r->r_offset - mbv)) != got_slot)
            continue;
        void *addr = resolve_jmp_symbol(obj, r);
        if (addr)
            *(uintptr_t *)got_slot = (uintptr_t)addr;
        else if (elf_debug()) {
            size_t sym_idx = ELF64_R_SYM(r->r_info);
            const char *nm = (sym_idx < obj->dynsym_count)
                                 ? obj->dynstr + obj->dynsym[sym_idx].st_name : "?";
            fprintf(stderr, "[!] lazy resolve FAILED: %s (obj %s)\n",
                    nm, obj->soname ? obj->soname : "?");
        }
        return addr;
    }
    if (elf_debug())
        fprintf(stderr, "[!] lazy: slot %p not in jmp_rela of %s\n",
                (void *)got_slot, obj->soname ? obj->soname : "?");
    return 0;
}

void elf_register_override(const char *name, void *fn) {
    if (!name || !fn || override_count >= MAX_OVERRIDES)
        return;
    for (size_t i = 0; i < override_count; i++) {
        if (strcmp(overrides[i].name, name) == 0) {
            overrides[i].fn = fn;
            return;
        }
    }
    overrides[override_count].name = name;
    overrides[override_count].fn = fn;
    override_count++;
}

static void *override_lookup(const char *name) {
    for (size_t i = 0; i < override_count; i++) {
        if (strcmp(overrides[i].name, name) == 0)
            return overrides[i].fn;
    }
    return NULL;
}

static size_t map_base_vaddr(const elf_object_t *obj) {
    size_t min_vaddr = SIZE_MAX;
    for (int i = 0; i < obj->phdr_count; i++) {
        if (obj->phdr[i].p_type == PT_LOAD && obj->phdr[i].p_vaddr < min_vaddr)
            min_vaddr = obj->phdr[i].p_vaddr;
    }
    return ALIGN_DOWN(min_vaddr, PAGE_SIZE);
}

static void *va(const elf_object_t *obj, size_t vaddr) {
    return (char *)obj->base_addr + (vaddr - map_base_vaddr(obj));
}

static uintptr_t read_tp(void) {
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

uintptr_t elf_read_tp(void) { return read_tp(); }

extern void tlsdesc_return(void);

static void *map_elf_segments(void *file_map, Elf64_Ehdr *ehdr, size_t *out_total,
                              size_t *out_min_vaddr) {
    Elf64_Phdr *fp = (Elf64_Phdr *)((char *)file_map + ehdr->e_phoff);
    size_t min_vaddr = SIZE_MAX;
    size_t max_vaddr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (fp[i].p_type == PT_LOAD) {
            if (fp[i].p_vaddr < min_vaddr)
                min_vaddr = fp[i].p_vaddr;
            size_t end = fp[i].p_vaddr + fp[i].p_memsz;
            if (end > max_vaddr)
                max_vaddr = end;
        }
    }
    if (min_vaddr == SIZE_MAX)
        return NULL;

    size_t mbv = ALIGN_DOWN(min_vaddr, PAGE_SIZE);
    size_t total = ALIGN_UP(max_vaddr, PAGE_SIZE) - mbv;
    void *base;
    if (ehdr->e_type == ET_EXEC) {
        base = mmap((void *)mbv, total, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (base == MAP_FAILED)
            return NULL;
    } else {
        base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED)
            return NULL;
    }

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (fp[i].p_type == PT_LOAD) {
            memcpy((char *)base + (fp[i].p_vaddr - mbv),
                   (char *)file_map + fp[i].p_offset, fp[i].p_filesz);
        }
    }
    if (out_total)
        *out_total = total;
    if (out_min_vaddr)
        *out_min_vaddr = mbv;
    return base;
}

static int load_table(const char *file_map, const Elf64_Ehdr *ehdr,
                      Elf64_Sym **sym_out, char **str_out, size_t *count_out, unsigned want) {
    Elf64_Shdr *file_shdr = (Elf64_Shdr *)((char *)file_map + ehdr->e_shoff);

    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (file_shdr[i].sh_type != want)
            continue;

        size_t count = file_shdr[i].sh_size / sizeof(Elf64_Sym);
        Elf64_Sym *sym = (Elf64_Sym *)malloc(file_shdr[i].sh_size);
        if (!sym)
            return -1;
        memcpy(sym, (char *)file_map + file_shdr[i].sh_offset, file_shdr[i].sh_size);

        int strtab_idx = file_shdr[i].sh_link;
        size_t strtab_size = file_shdr[strtab_idx].sh_size;
        char *str = (char *)malloc(strtab_size);
        if (!str) {
            free(sym);
            return -1;
        }
        memcpy(str, (char *)file_map + file_shdr[strtab_idx].sh_offset, strtab_size);

        *sym_out = sym;
        *str_out = str;
        *count_out = count;
        return 0;
    }
    return 1;
}

static Elf64_Dyn *find_dynamic(const elf_object_t *obj) {
    for (int i = 0; i < obj->phdr_count; i++) {
        if (obj->phdr[i].p_type == PT_DYNAMIC)
            return (Elf64_Dyn *)va(obj, obj->phdr[i].p_vaddr);
    }
    return NULL;
}

static char *expand_dirs(const char *list, const char *origin_dir) {
    size_t cap = strlen(list) + 2048;
    char *out = calloc(1, cap);
    size_t o = 0;
    /* rootfs_base = origin_dir bez trailing /usr/bin nebo /bin.
     * Absolutni RUNPATH/RPATH (napr. /usr/lib/aarch64-linux-gnu/systemd
     * u systemd binarek) jsou relativni k ROOTFS, ne k device rootu -
     * bez toho je loader hleda na Android /usr/lib a nenajde je. */
    char *rootfs_base = NULL;
    if (origin_dir) {
        size_t ol = strlen(origin_dir);
        if (ol >= 8 && strcmp(origin_dir + ol - 8, "/usr/bin") == 0)
            rootfs_base = strndup(origin_dir, ol - 8);
        else if (ol >= 4 && strcmp(origin_dir + ol - 4, "/bin") == 0)
            rootfs_base = strndup(origin_dir, ol - 4);
        else
            rootfs_base = strdup("/");
    }
    while (*list) {
        const char *semi = strchr(list, ':');
        size_t len = semi ? (size_t)(semi - list) : strlen(list);
        char *item = strndup(list, len);
        char *p = item;
        /* Absolutni cesta v RUNPATH -> prepend rootfs_base */
        if (rootfs_base && item[0] == '/') {
            size_t bl = strlen(rootfs_base);
            if (o + bl + 1 < cap) {
                memcpy(out + o, rootfs_base, bl);
                o += bl;
            }
        }
        while (*p) {
            if (strncmp(p, "$ORIGIN", 7) == 0) {
                size_t d = strlen(origin_dir);
                if (o + d + 2 > cap) break;
                memcpy(out + o, origin_dir, d);
                o += d;
                p += 7;
            } else if (strncmp(p, "${ORIGIN}", 9) == 0) {
                size_t d = strlen(origin_dir);
                if (o + d + 2 > cap) break;
                memcpy(out + o, origin_dir, d);
                o += d;
                p += 9;
            } else if (strncmp(p, "$LIB", 4) == 0) {
                if (o + 4 > cap) break;
                memcpy(out + o, "lib", 3);
                o += 3;
                p += 4;
            } else if (strncmp(p, "$PLATFORM", 9) == 0) {
                if (o + 8 > cap) break;
                memcpy(out + o, "aarch64", 7);
                o += 7;
                p += 9;
            } else {
                if (o + 1 >= cap) break;
                out[o++] = *p++;
            }
        }
        free(item);
        if (o + 1 < cap)
            out[o++] = ':';
        if (semi)
            list = semi + 1;
        else
            break;
    }
    if (o > 0)
        out[o - 1] = '\0';
    free(rootfs_base);
    return out;
}

static void *dlopen_search(const char *soname, const char *paths) {
    void *h;
    const char *p = paths;
    while (p && *p) {
        const char *semi = strchr(p, ':');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len > 0) {
            char *cand = malloc(len + strlen(soname) + 2);
            memcpy(cand, p, len);
            cand[len] = '/';
            strcpy(cand + len + 1, soname);
            h = dlopen(cand, RTLD_NOW | RTLD_GLOBAL);
            free(cand);
            if (h)
                return h;
        }
        if (semi)
            p = semi + 1;
        else
            break;
    }
    return NULL;
}

static int load_needed(elf_object_t *obj) {
    Elf64_Dyn *dyn = find_dynamic(obj);
    if (!dyn)
        return 0;

    char *dynstr = NULL;
    int count = 0;

    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_STRTAB)
            dynstr = va(obj, d->d_un.d_ptr);
        else if (d->d_tag == DT_NEEDED)
            count++;
    }
    if (!dynstr || count == 0)
        return 0;

    char *runpath = NULL;
    char *rpath = NULL;
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RUNPATH)
            runpath = dynstr + d->d_un.d_val;
        else if (d->d_tag == DT_RPATH)
            rpath = dynstr + d->d_un.d_val;
    }

    char *exp = NULL;
    if (runpath)
        exp = expand_dirs(runpath, obj->origin_dir);
    else if (rpath)
        exp = expand_dirs(rpath, obj->origin_dir);

    char *env_paths = getenv("LD_LIBRARY_PATH");
    size_t exp_len = exp ? strlen(exp) : 0;
    size_t env_len = env_paths ? strlen(env_paths) : 0;
    size_t bin_len = strlen(obj->origin_dir);
    char *search = malloc(exp_len + env_len + bin_len + 32);
    search[0] = '\0';
    if (exp && exp_len)
        memcpy(search, exp, exp_len);
    if (env_paths && env_len) {
        if (search[0])
            strcat(search, ":");
        strcat(search, env_paths);
    }
    if (search[0])
        strcat(search, ":");
    strcat(search, obj->origin_dir);

    void **handles = calloc(count, sizeof(void *));
    if (!handles) {
        free(search);
        free(exp);
        return -1;
    }

    int n = 0;
    if (elf_own_deps && obj->scope) {
        char *dl = derive_distro_libdirs(obj->origin_dir ? obj->origin_dir : "");
        char *osearch;
        if (dl) {
            osearch = malloc(strlen(dl) + strlen(search) +
                             strlen(sys_libdirs()) + 4);
            sprintf(osearch, "%s:%s:%s", dl, search, sys_libdirs());
        } else {
            osearch = malloc(strlen(search) + strlen(sys_libdirs()) + 8);
            sprintf(osearch, "%s:%s", search, sys_libdirs());
        }
        /* distro ld.so do scope PŘED ostatními deps (libc importuje
           GLIBC_PRIVATE symboly z ld.so) */
        preload_distro_ldso(osearch, obj->scope);
        if (elf_debug()) printf("[dbg] preload ok\n");
        for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag != DT_NEEDED)
                continue;
            const char *soname = dynstr + d->d_un.d_val;
            if (elf_debug()) printf("[dbg] needed=%s\n", soname);
            if (is_ld_linux(soname)) {
                if (elf_debug())
                    printf("[+] dep %s: host ld-linux fallback\n", soname);
                continue;
            }
            char *cand = find_in_paths(soname, osearch);
            if (cand) {
                if (elf_debug())
                    printf("[+] own-loading dependency: %s\n", cand);
                elf_load_shared(cand, obj->scope);
                free(cand);
            } else {
                fprintf(stderr, "[-] dep %s not found\n", soname);
                if (is_core_lib(soname))
                    fatal_missing_dep(soname, osearch);
            }
        }
        free(osearch);
        free(search);
        free(exp);
        return 0;
    }

    {
        char *dl2 = derive_distro_libdirs(obj->origin_dir ? obj->origin_dir : "");
        if (dl2) {
            char *ns = malloc(strlen(search) + strlen(dl2) + 2);
            sprintf(ns, "%s:%s", dl2, search);
            free(search);
            search = ns;
        }
    }
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED)
            continue;
        const char *soname = dynstr + d->d_un.d_val;
        void *h = dlopen_search(soname, search);
        if (!h)
            h = dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            return 0;
        if (elf_debug())
            printf("[+] loaded dependency: %s\n", soname);
        handles[n++] = h;
    }

    free(search);
    free(exp);
    obj->handles = handles;
    obj->handle_count = count;
    return 0;
}

static int is_android_stub(const char *path);

elf_object_t *elf_load(const char *path) {
    if (is_android_stub(path)) {
        fprintf(stderr,
                "[-] %s: Android stub (symlink to /bin/true) — not a runnable binary.\n"
                "    Use the real distro binary, or run via: gbsh --chroot <rootfs> <bin>\n",
                path);
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[-] open(%s): %s\n", path ? path : "(null)", strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return NULL;
    }

    void *file_map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_map == MAP_FAILED) {
        perror("mmap file");
        close(fd);
        return NULL;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_map;

    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "[-] Not an ELF file\n");
        goto cleanup;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "[-] Only ELF64 supported\n");
        goto cleanup;
    }

    Elf64_Phdr *file_phdr = (Elf64_Phdr *)((char *)file_map + ehdr->e_phoff);

    size_t total_size = 0, map_base_vaddr_ = 0;
    void *base = map_elf_segments(file_map, ehdr, &total_size, &map_base_vaddr_);
    if (!base) {
        fprintf(stderr, "[-] No LOAD segments\n");
        goto cleanup;
    }

    elf_object_t *obj = calloc(1, sizeof(elf_object_t));
    if (!obj) {
        munmap(base, total_size);
        goto cleanup;
    }

    obj->base_addr = base;
    obj->total_size = total_size;
    obj->phdr_count = ehdr->e_phnum;
    obj->entry_point = (void *)((char *)base + (ehdr->e_entry - map_base_vaddr_));

    {
        const char *slash = strrchr(path, '/');
        if (slash)
            obj->origin_dir = strndup(path, slash - path);
        else
            obj->origin_dir = strdup(".");
    }

    obj->ehdr = (Elf64_Ehdr *)malloc(sizeof(Elf64_Ehdr));
    memcpy(obj->ehdr, ehdr, sizeof(Elf64_Ehdr));

    if (elf_own_deps && elf_own_scope)
        obj->scope = elf_own_scope;

    obj->phdr = (Elf64_Phdr *)malloc(sizeof(Elf64_Phdr) * ehdr->e_phnum);
    memcpy(obj->phdr, file_phdr, sizeof(Elf64_Phdr) * ehdr->e_phnum);

    maybe_fixup_bionic_phdr(obj);

    for (int i = 0; i < obj->phdr_count; i++) {
        if (obj->phdr[i].p_type != PT_TLS)
            continue;
        obj->tls_offset = 0x10;
        obj->tls_memsz = obj->phdr[i].p_memsz;
        obj->has_tls = 1;
        break;
    }

    if (load_table(file_map, ehdr, &obj->symtab, &obj->strtab,
                   &obj->symtab_count, SHT_SYMTAB) < 0)
        goto cleanup_obj;
    if (load_table(file_map, ehdr, &obj->dynsym, &obj->dynstr,
                   &obj->dynsym_count, SHT_DYNSYM) < 0)
        goto cleanup_obj;

    if (elf_debug()) printf("[dbg3] tls-block done\n");
    munmap(file_map, st.st_size);
    close(fd);

    if (load_needed(obj) < 0) {
        elf_unload(obj);
        return NULL;
    }
    return obj;

cleanup_obj:
    free(obj->symtab);
    free(obj->strtab);
    free(obj->dynsym);
    free(obj->dynstr);
    free(obj->ehdr);
    free(obj->phdr);
    free(obj);
cleanup:
    munmap(file_map, st.st_size);
    close(fd);
    return NULL;
}

/* ---- in-process ELF shared-object loader + private scope ---- */

elf_scope_t *elf_scope_create(void) {
    return calloc(1, sizeof(elf_scope_t));
}

void elf_scope_destroy(elf_scope_t *s) {
    if (!s)
        return;
    for (size_t i = 0; i < s->count; i++)
        if (s->mods[i])
            elf_unload(s->mods[i]);
    free(s->mods);
    free(s);
}

void elf_scope_add(elf_scope_t *s, elf_object_t *m) {
    if (!s || !m)
        return;
    if (s->count == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 8;
        elf_object_t **nmods = realloc(s->mods, ncap * sizeof(*nmods));
        if (!nmods)
            return;
        s->mods = nmods;
        s->cap = ncap;
    }
    s->mods[s->count++] = m;
}

void *elf_scope_lookup(const elf_scope_t *s, const char *name) {
    const Elf64_Sym *sym = NULL;
    elf_object_t *m = elf_scope_find(s, name, &sym);
    if (m && sym)
        return (char *)m->base_addr + (sym->st_value - map_base_vaddr(m));
    return NULL;
}

elf_object_t *elf_scope_find(const elf_scope_t *s, const char *name,
                             const Elf64_Sym **out_sym) {
    if (!s || !name)
        return NULL;
    for (size_t i = 0; i < s->count; i++) {
        elf_object_t *m = s->mods[i];
        if (!m || !m->dynsym || !m->dynstr)
            continue;
        for (size_t j = 0; j < m->dynsym_count; j++) {
            const Elf64_Sym *sym = &m->dynsym[j];
            if (sym->st_name == 0 || sym->st_shndx == SHN_UNDEF)
                continue;
            if (ELF64_ST_BIND(sym->st_info) == STB_LOCAL)
                continue;
            if (strcmp(m->dynstr + sym->st_name, name) != 0)
                continue;
            if (out_sym)
                *out_sym = sym;
            return m;
        }
    }
    if (out_sym)
        *out_sym = NULL;
    return NULL;
}

static char *build_search(const char *origin_dir) {
    char *env_paths = getenv("LD_LIBRARY_PATH");
    size_t env_len = env_paths ? strlen(env_paths) : 0;
    size_t bin_len = strlen(origin_dir);
    char *search = malloc(bin_len + env_len + 32);
    search[0] = '\0';
    if (env_paths && env_len) {
        memcpy(search, env_paths, env_len);
        strcat(search, ":");
    }
    strcat(search, origin_dir);
    return search;
}

/* Odvození distro lib cest z origin_dir binárky v distro layoutu:
 *   …/distro/usr/bin/exe → …/distro/{usr/lib/aarch64-linux-gnu,
 *   lib/aarch64-linux-gnu, usr/lib, lib}
 * Umožňuje najít libc.so.6 apod. bez LD_LIBRARY_PATH i bez ELF_ROOTFS. */
static char *derive_distro_libdirs(const char *origin_dir) {
    static char buf[2048];
    if (!origin_dir) return NULL;
    size_t l = strlen(origin_dir);
    /* rootfs base = prefix pred PRVNIM /usr, /lib nebo /bin komponentou.
     * Plati pro exe (/distro/usr/bin) i pro sdilene knihovny
     * (/distro/usr/lib/aarch64-linux-gnu/systemd/libfoo.so), takze i jejich
     * transitivni deps (libacl, libblkid, ...) se najdou v standardnim libdiru. */
    size_t bl = 0;
    for (size_t i = 0; i < l; i++) {
        if ((i == 0 || origin_dir[i - 1] == '/') &&
            ((origin_dir[i] == 'u' && strncmp(origin_dir + i, "usr", 3) == 0 &&
              (origin_dir[i + 3] == '/' || origin_dir[i + 3] == 0)) ||
             (origin_dir[i] == 'l' && strncmp(origin_dir + i, "lib", 3) == 0 &&
              (origin_dir[i + 3] == '/' || origin_dir[i + 3] == 0)) ||
             (origin_dir[i] == 'b' && strncmp(origin_dir + i, "bin", 3) == 0 &&
              (origin_dir[i + 3] == '/' || origin_dir[i + 3] == 0)))) {
            bl = i;
            break;
        }
    }
    if (bl == 0)
        bl = l; /* zadna standardni komponenta -> cely adresar */

    if (bl + 256 >= sizeof buf) return NULL;
    memcpy(buf, origin_dir, bl); buf[bl] = 0;

    /* pořadí: multiarch, lib, usr/lib, lib — nejlepší match první */
    char tmp[2048];
    snprintf(tmp, sizeof tmp,
             "%s/usr/lib/aarch64-linux-gnu:%s/lib/aarch64-linux-gnu:"
             "%s/usr/lib:%s/lib",
             buf, buf, buf, buf);
    snprintf(buf, sizeof buf, "%s", tmp);
    return buf;
}

static int g_ldso_preloaded = 0;

static void preload_distro_ldso(const char *osearch, elf_scope_t *scope) {
    if (!elf_own_deps || !scope || g_ldso_preloaded)
        return;
    if (getenv("ELF_LOADER_NO_LDSO_PRELOAD"))
        return;
    char *ldc = find_in_paths("ld-linux-aarch64.so.1", osearch);
    if (!ldc)
        return;
    g_ldso_preloaded = 1; /* před loadem — load_module_needed(ld.so) by
                             jinak rekurzoval donekonečna */
    if (elf_debug())
        printf("[+] own-loading %s (distro ld.so)\n", ldc);
    elf_load_shared(ldc, scope);
    free(ldc);
}

static char *find_in_paths(const char *soname, const char *search) {
    const char *p = search;
    while (p && *p) {
        const char *semi = strchr(p, ':');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len > 0) {
            char *cand = malloc(len + strlen(soname) + 2);
            memcpy(cand, p, len);
            cand[len] = '/';
            strcpy(cand + len + 1, soname);
            if (access(cand, R_OK) == 0)
                return cand;
            free(cand);
        }
        if (semi)
            p = semi + 1;
        else
            break;
    }
    return NULL;
}

static int load_module_needed(elf_object_t *m, elf_scope_t *scope) {
    Elf64_Dyn *dyn = find_dynamic(m);
    if (!dyn)
        return 0;

    char *dynstr = NULL;
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++)
        if (d->d_tag == DT_STRTAB)
            dynstr = va(m, d->d_un.d_ptr);
    if (!dynstr)
        return 0;

    char *search = build_search(m->origin_dir ? m->origin_dir : ".");
    if (elf_own_deps) {
        char *dl = derive_distro_libdirs(m->origin_dir ? m->origin_dir : "");
        char *osearch;
        if (dl) {
            osearch = malloc(strlen(dl) + strlen(search) +
                             strlen(sys_libdirs()) + 4);
            sprintf(osearch, "%s:%s:%s", dl, search, sys_libdirs());
        } else {
            osearch = malloc(strlen(search) + strlen(sys_libdirs()) + 8);
            sprintf(osearch, "%s:%s", search, sys_libdirs());
        }
        preload_distro_ldso(osearch, scope);
        for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag != DT_NEEDED)
                continue;
            const char *soname = dynstr + d->d_un.d_val;
            if (is_ld_linux(soname))
                continue;
            char *cand = find_in_paths(soname, osearch);
            if (cand) {
                if (elf_debug())
                    printf("[+] own-loading dependency: %s\n", cand);
                elf_load_shared(cand, scope);
                free(cand);
            } else {
                fprintf(stderr, "[-] module dep %s: not found\n", soname);
                if (is_core_lib(soname))
                    fatal_missing_dep(soname, osearch);
            }
        }
        free(osearch);
        free(search);
        return 0;
    }
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED)
            continue;
        const char *soname = dynstr + d->d_un.d_val;
        void *h = dlopen_search(soname, search);
        if (!h)
            h = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
        if (h)
            continue;
        char *cand = find_in_paths(soname, search);
        if (cand) {
            if (elf_debug())
                printf("[+] own-loading dependency: %s\n", cand);
            elf_object_t *dep = elf_load_shared(cand, scope);
            free(cand);
            if (dep)
                continue;
        }
        fprintf(stderr, "[-] module dep %s: not found\n", soname);
        if (is_core_lib(soname))
            fatal_missing_dep(soname, search);
    }
    free(search);
    return 0;
}

/* Fronta init funkcí (DT_INIT + init_array všech own-loadených modulů).
 * Nesmí se volat v loader fázi pod host bionic TP: libstdc++/threadové knihovny
 * (btop, apt) v ctorusech sahají pod TP-0x720 (_pthread_cleanup_push /
 * cancellable futex) -> guard page bionického main-TLS -> SIGSEGV.
 * Spouští je elf_run_final() až POD parrot TP těsně před entry. */
typedef void (*init_fn_t)(int, char **, char **);
static init_fn_t *g_pending_inits;
static size_t g_pending_count, g_pending_cap;

/* libc TLS per-thread state: locale pointer + ctype tables leží v
 * [TP + slot_off] slotech (offsety v libc .data na 0x1aff40 / 0x1afd58).
 * Náš region je zeroed -> strtol/isalpha atd. dereferencují NULL.
 * Resolvujeme uselocale(NULL) (= global locale) a __ctype_init()
 * přímo z own-loadeného libc a voláme pod parrot TP. */
static void *(*g_libc_uselocale)(void *);
static void (*g_libc_ctype_init)(void);
extern uintptr_t g_tls_new_tp;

/* Volá se z asm (elf_final_jump) pod parrot TP. Žádný bionic kód/malloc. */
void elf_run_pending_inits(void) {
    if (g_tls_new_tp) {
        if (!getenv("ELF_LOADER_NO_LOCALE")) {
            if (g_libc_uselocale)
                g_libc_uselocale(NULL);      /* thread locale = _nl_global_locale */
            if (g_libc_ctype_init)
                g_libc_ctype_init();         /* ctype_b/tolower sloty pro tento TP */
        }
    }
    if (!getenv("ELF_LOADER_NO_INITS"))
    for (size_t i = 0; i < g_pending_count; i++) {
        init_fn_t fn = g_pending_inits[i];
        fn(elf_init_argc, elf_init_argv, elf_init_envp);
    }
    /* po initech: libc/program může mít přepsán SIGSEGV handler (procps
     * ps/top) → reinstalovat náš fault dump handler pro diagnostiku */
    if (getenv("ELF_LOADER_KEEP_HANDLERS"))
        elf_install_fault_handlers();
}

static __attribute__((noreturn)) void elf_run_final(void *sp, void *entry, elf_object_t *obj);

static void elf_queue_init(init_fn_t fn) {
    if (!fn)
        return;
    if (g_pending_count == g_pending_cap) {
        size_t nc = g_pending_cap ? g_pending_cap * 2 : 32;
        init_fn_t *np = realloc(g_pending_inits, nc * sizeof(init_fn_t));
        if (!np)
            return;
        g_pending_inits = np;
        g_pending_cap = nc;
    }
    g_pending_inits[g_pending_count++] = fn;
}

static sym_status_t lookup_table(const Elf64_Sym *symtab, const char *strtab,
                                 size_t count, const char *name, void **out_addr,
                                 const elf_object_t *obj);

static void run_module_init(elf_object_t *m) {
    Elf64_Dyn *dyn = find_dynamic(m);
    if (!dyn)
        return;
    /* glibc's __libc_early_init() (normally called by ld.so at exec with
     * arg=1) sets the flag byte at libc+0x1be009; strerror_l reads its bit 0
     * to pick the "Unknown error %d" (asprintf) path vs. the plain static
     * string. Our loader never runs the real early-init, so we set the flag
     * ourselves to keep strerror/perror output identical to host glibc. */
    if (m->soname && strcmp(m->soname, "libc.so.6") == 0) {
        /* Flag byte __libc_early_init: strerror_l čte bit 0 → cesta
         * "Unknown error %d". Skutečné ei(1) volat NEMŮŽEME — sahá na
         * GLRO struktury naší ld.so simulace (SIGSEGV). Hardcoded offset
         * platí pro glibc 2.41 build; guard: symbol musí existovat
         * (glibc ≥ 2.34; starší 2.28 rootfy ho nemají → skip) a adresa
         * musí ležet v mapování modulu (jinak kosmetická odchylka). */
        void *ei = NULL;
        if (lookup_table(m->dynsym, m->dynstr, m->dynsym_count,
                         "__libc_early_init", &ei, m) == SYM_DEFINED && ei &&
            m->total_size > 0x1be009 + 1) {
            *(char *)va(m, 0x1be009) = 1;
        }
        /* uselocale/__ctype_init pro TLS per-thread state (viz výše) */
        void *a = NULL;
        if (lookup_table(m->dynsym, m->dynstr, m->dynsym_count,
                         "uselocale", &a, m) == SYM_DEFINED && a)
            g_libc_uselocale = (void *(*)(void *))a;
        a = NULL;
        if (lookup_table(m->dynsym, m->dynstr, m->dynsym_count,
                         "__ctype_init", &a, m) == SYM_DEFINED && a)
            g_libc_ctype_init = (void (*)(void))a;
    }
    uint64_t init = 0, init_array = 0, init_arraysz = 0;
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_INIT)
            init = d->d_un.d_ptr;
        else if (d->d_tag == DT_INIT_ARRAY)
            init_array = d->d_un.d_ptr;
        else if (d->d_tag == DT_INIT_ARRAYSZ)
            init_arraysz = d->d_un.d_val;
    }
    typedef void (*init_fn_t)(int, char **, char **);
    if (init)
        elf_queue_init((init_fn_t)va(m, init));
    if (init_array && init_arraysz) {
        uint64_t *arr = (uint64_t *)va(m, init_array);
        size_t n = init_arraysz / sizeof(uint64_t);
        for (size_t i = 0; i < n; i++)
            elf_queue_init((init_fn_t)arr[i]);
    }
}

/* Android stub detection: a dependency symlinked to /bin/true (or
   /system/bin/true) is an Android stub placeholder, not a real shared
   object. Loading it crashes with a cryptic SIGSYS/SEGV. Detect and
   report clearly so the user knows to use gbsh --chroot or provide the
   real distro library. */
static int is_android_stub(const char *path) {
    if (!path)
        return 0;
    /* Android's stub libs are symlinks to /bin/true (the stub marker).
       A real /bin/true is a normal executable, not a stub, so we only
       flag actual symlinks whose target is a *true binary. */
    char linkbuf[PATH_MAX];
    ssize_t n = readlink(path, linkbuf, sizeof(linkbuf) - 1);
    if (n <= 0)
        return 0;
    linkbuf[n] = '\0';
    const char *lb = strrchr(linkbuf, '/');
    lb = lb ? lb + 1 : linkbuf;
    return strcmp(lb, "true") == 0;
}

elf_object_t *elf_load_shared(const char *path, elf_scope_t *scope) {
    if (is_android_stub(path)) {
        fprintf(stderr,
                "[-] %s: Android stub (symlink to /bin/true) — cannot own-load.\n"
                "    Use the real distro library, or run via: gbsh --chroot <rootfs> <bin>\n",
                path);
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[-] open(%s): %s\n", path ? path : "(null)", strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return NULL;
    }
    void *file_map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_map == MAP_FAILED) {
        perror("mmap file");
        close(fd);
        return NULL;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_map;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_type != ET_DYN) {
        fprintf(stderr, "[-] %s: not an ELF64 shared object\n", path);
        munmap(file_map, st.st_size);
        close(fd);
        return NULL;
    }

    const char *slash = strrchr(path, '/');
    const char *base_name = slash ? slash + 1 : path;
    char *soname = NULL;
    {
        Elf64_Phdr *fph = (Elf64_Phdr *)((char *)file_map + ehdr->e_phoff);
        Elf64_Phdr *dynph = NULL;
        for (int i = 0; i < ehdr->e_phnum; i++)
            if (fph[i].p_type == PT_DYNAMIC)
                dynph = &fph[i];
        if (dynph) {
            Elf64_Dyn *dyn = (Elf64_Dyn *)((char *)file_map +
                                           dynph->p_offset);
            const char *dynstr = NULL;
            size_t strtab_vaddr = 0;
            for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
                if (d->d_tag == DT_STRTAB)
                    strtab_vaddr = d->d_un.d_ptr;
                else if (d->d_tag == DT_SONAME && strtab_vaddr) {
                    for (int i = 0; i < ehdr->e_phnum; i++) {
                        if (fph[i].p_type == PT_LOAD &&
                            strtab_vaddr >= fph[i].p_vaddr &&
                            strtab_vaddr < fph[i].p_vaddr + fph[i].p_memsz) {
                            dynstr = (char *)file_map + strtab_vaddr -
                                     (fph[i].p_vaddr - fph[i].p_offset);
                            break;
                        }
                    }
                    if (dynstr)
                        soname = strdup(dynstr + d->d_un.d_val);
                    break;
                }
            }
        }
    }
    if (!soname)
        soname = strdup(base_name);

    for (size_t i = 0; i < scope->count; i++) {
        if (scope->mods[i]->soname &&
            strcmp(scope->mods[i]->soname, soname) == 0) {
            free(soname);
            munmap(file_map, st.st_size);
            close(fd);
            return scope->mods[i];
        }
    }

    size_t total_size = 0, mbv = 0;
    void *base = map_elf_segments(file_map, ehdr, &total_size, &mbv);
    loader_phase = "loading";
    if (!base) {
        fprintf(stderr, "[-] %s: no LOAD segments\n", path);
        munmap(file_map, st.st_size);
        close(fd);
        return NULL;
    }

    elf_object_t *m = calloc(1, sizeof(elf_object_t));
    if (!m) {
        munmap(base, total_size);
        munmap(file_map, st.st_size);
        close(fd);
        return NULL;
    }
    m->base_addr = base;
    m->total_size = total_size;
    m->phdr_count = ehdr->e_phnum;
    m->scope = scope;
    m->soname = soname;
    {
        const char *slash = strrchr(path, '/');
        m->origin_dir = slash ? strndup(path, slash - path) : strdup(".");
    }
    m->ehdr = malloc(sizeof(Elf64_Ehdr));
    memcpy(m->ehdr, ehdr, sizeof(Elf64_Ehdr));
    m->phdr = malloc(sizeof(Elf64_Phdr) * ehdr->e_phnum);
    memcpy(m->phdr, (Elf64_Phdr *)((char *)file_map + ehdr->e_phoff),
           sizeof(Elf64_Phdr) * ehdr->e_phnum);

    load_table(file_map, ehdr, &m->dynsym, &m->dynstr, &m->dynsym_count,
               SHT_DYNSYM);
    if (elf_debug()) printf("[dbg3] dynsym %s: %zu\n", path, m->dynsym_count);

    for (int i = 0; i < m->phdr_count; i++) {
        if (m->phdr[i].p_type != PT_TLS)
            continue;
        if (elf_debug())
            printf("[dbg3] tls phdr %s: filesz=%llx memsz=%llx align=%llx\n",
                   path, (unsigned long long)m->phdr[i].p_filesz,
                   (unsigned long long)m->phdr[i].p_memsz,
                   (unsigned long long)m->phdr[i].p_align);
        size_t sz = ALIGN_UP(m->phdr[i].p_memsz, m->phdr[i].p_align);
        void *blk = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (blk != MAP_FAILED) {
            memset(blk, 0, sz);
            /* Inicializacni image .tdata z ELF (ne z host TP!): arena/locale
             * pointery libc jsou v .tdata - pod bionickym host TP by kopie
             * z host TLS byla garbage. */
            memcpy(blk, (char *)base + (m->phdr[i].p_vaddr - mbv),
                   m->phdr[i].p_filesz);
            m->tls_offset = (uintptr_t)blk - read_tp();
            m->tls_memsz = m->phdr[i].p_memsz;
            m->tdata_src = blk;   /* blk[0..filesz] = ELF .tdata image */
            m->has_tls = 1;
        }
        break;
    }

    munmap(file_map, st.st_size);
    close(fd);

    load_module_needed(m, scope);
    elf_scope_add(scope, m);
    if (elf_debug()) printf("[dbg2] reloc-start %s\n", path);
    elf_relocate(m);
    if (elf_debug()) printf("[dbg2] reloc-done\n");

    patch_module_heap_syms(m);
    if (elf_debug()) printf("[dbg2] heap-syms-done\n");

    run_module_init(m);
    if (elf_debug()) printf("[dbg2] init-queued\n");

    for (int i = 0; i < m->phdr_count; i++) {
        if (m->phdr[i].p_type != PT_TLS || !m->has_tls)
            continue;
        char *src = (char *)base + (m->phdr[i].p_vaddr - mbv);
        if (elf_debug())
            printf("[dbg2] tls-copy off=%llx filesz=%llx memsz=%llx\n",
                   (unsigned long long)m->tls_offset,
                   (unsigned long long)m->phdr[i].p_filesz,
                   (unsigned long long)m->phdr[i].p_memsz);
        memcpy((void *)read_tp() + m->tls_offset, src, m->phdr[i].p_filesz);
        break;
    }
    if (elf_debug()) printf("[dbg2] tls-done\n");

    for (size_t j = 0; j < m->dynsym_count; j++) {
        const Elf64_Sym *sym = &m->dynsym[j];
        if (sym->st_shndx == SHN_UNDEF)
            continue;
        const char *nm = m->dynstr + sym->st_name;
        if (strcmp(nm, "__environ") == 0 || strcmp(nm, "environ") == 0 ||
            strcmp(nm, "_environ") == 0) {
            *(uintptr_t *)((char *)base + (sym->st_value - mbv)) =
                (uintptr_t)environ;
            break;
        }
    }
    for (size_t j = 0; j < m->dynsym_count; j++) {
        const Elf64_Sym *sym = &m->dynsym[j];
        if (sym->st_shndx == SHN_UNDEF)
            continue;
        const char *nm = m->dynstr + sym->st_name;
        if (strcmp(nm, "__curbrk") == 0 || strcmp(nm, "___brk_addr") == 0) {
            *(uintptr_t *)((char *)base + (sym->st_value - mbv)) =
                (uintptr_t)ldso_sbrk(0);
            break;
        }
    }
    if (elf_debug())
        printf("[+] own-loaded module: %s (base %p, %zu dynsym)\n", path,
           (void *)base, m->dynsym_count);
    fflush(stdout);
    return m;
}

static sym_status_t lookup_table(const Elf64_Sym *symtab, const char *strtab,
                                 size_t count, const char *name, void **out_addr,
                                 const elf_object_t *obj) {
    if (!symtab || !strtab)
        return SYM_NOT_FOUND;

    for (size_t i = 0; i < count; i++) {
        if (symtab[i].st_name == 0)
            continue;
        const char *sym_name = strtab + symtab[i].st_name;
        if (strcmp(sym_name, name) != 0)
            continue;

        if (symtab[i].st_shndx == SHN_UNDEF) {
            if (out_addr)
                *out_addr = NULL;
            return SYM_IMPORT;
        }
        if (out_addr)
            *out_addr = va(obj, symtab[i].st_value);
        return SYM_DEFINED;
    }
    return SYM_NOT_FOUND;
}

void *elf_resolve_import(elf_object_t *obj, const char *name) {
    void *sym = resolve_import_ldso(name);
    if (sym)
        return sym;
    sym = override_lookup(name);
    if (sym)
        return sym;
    if (obj && obj->scope) {
        sym = elf_scope_lookup(obj->scope, name);
        if (sym)
            return sym;
    }
    for (size_t i = 0; obj && i < obj->handle_count; i++) {
        if (!obj->handles[i])
            continue;
        void *h = dlsym(obj->handles[i], name);
        if (h)
            return h;
    }
    /* Host fallback jen pro non-ownall flow (--run/--own/--shim): proces
     * běží pod host libc, takže její symboly (printf, __libc_start_main...)
     * můžeme použít přímo. --ownall (parrot svět) musí zůstat strict —
     * tam by bionic symboly tiše rozbily glibc program. */
    if (!elf_own_deps) {
        void *h2 = dlsym(RTLD_DEFAULT, name);
        if (h2)
            return h2;
    }
    return NULL;
}

sym_status_t elf_resolve_symbol(elf_object_t *obj, const char *name, void **out_addr) {
    if (!obj || !name)
        return SYM_NOT_FOUND;

    sym_status_t st = lookup_table(obj->symtab, obj->strtab, obj->symtab_count,
                                   name, out_addr, obj);
    if (st != SYM_NOT_FOUND)
        return st;
    st = lookup_table(obj->dynsym, obj->dynstr, obj->dynsym_count,
                      name, out_addr, obj);

    if (st == SYM_DEFINED)
        return st;

    void *imp = elf_resolve_import(obj, name);
    if (imp) {
        if (out_addr)
            *out_addr = imp;
        return SYM_IMPORT;
    }
    if (st == SYM_IMPORT) {
        if (out_addr)
            *out_addr = NULL;
        return SYM_IMPORT;
    }
    return SYM_NOT_FOUND;
}

static void relocate_tls(elf_object_t *obj, Elf64_Rela *r, uint64_t *where) {
    size_t sym_idx = ELF64_R_SYM(r->r_info);
    elf_object_t *dm = obj;
    uintptr_t sym_off = 0;
    if (sym_idx < obj->dynsym_count) {
        const Elf64_Sym *s = &obj->dynsym[sym_idx];
        if (s->st_shndx != SHN_UNDEF) {
            sym_off = s->st_value;
        } else if (s->st_name && obj->scope) {
            const Elf64_Sym *os = NULL;
            elf_object_t *dm2 =
                elf_scope_find(obj->scope, obj->dynstr + s->st_name, &os);
            if (dm2 && os) {
                dm = dm2;
                sym_off = os->st_value;
            }
        }
    }
    uintptr_t off = sym_off + r->r_addend + (dm->has_tls ? dm->tls_offset : 0);
    if (elf_debug())
        fprintf(stderr,
                "[tls] %s sym_off=%llx addend=%llx dm=%s has_tls=%d -> %llx\n",
                ELF64_R_TYPE(r->r_info) == R_AARCH64_TLS_TPREL ? "TPREL"
                                                                : "TLSDESC",
                (unsigned long long)sym_off,
                (unsigned long long)r->r_addend,
                dm && dm->soname ? dm->soname : "EXE",
                dm ? dm->has_tls : -1, (unsigned long long)off);
    if (ELF64_R_TYPE(r->r_info) == R_AARCH64_TLS_TPREL)
        *where = off;
    else {
        where[0] = (uint64_t)tlsdesc_return;
        where[1] = off;
    }
}

static void apply_relr(elf_object_t *obj) {
    Elf64_Dyn *dyn = find_dynamic(obj);
    if (!dyn)
        return;
    void *relr = NULL;
    size_t relrsz = 0;
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELR)
            relr = va(obj, d->d_un.d_ptr);
        else if (d->d_tag == DT_RELRSZ)
            relrsz = d->d_un.d_val;
    }
    if (!relr || !relrsz)
        return;
    uint64_t *where = NULL;
    uint64_t *r = (uint64_t *)relr;
    uint64_t *end = (uint64_t *)((char *)relr + relrsz);
    uintptr_t l_addr = (uintptr_t)obj->base_addr;
    for (; r < end; r++) {
        uint64_t entry = *r;
        if ((entry & 1) == 0) {
            where = (uint64_t *)(l_addr + entry);
            *where++ += l_addr;
        } else {
            for (long i = 0; (entry >>= 1) != 0; i++)
                if ((entry & 1) != 0)
                    where[i] += l_addr;
            where += CHAR_BIT * sizeof(uint64_t) - 1;
        }
    }
}

int elf_relocate(elf_object_t *obj) {
    if (!obj)
        return -1;
    if (obj->relocated)
        return 0;

    loader_phase = obj->soname ? obj->soname : "exe";

    Elf64_Dyn *dyn = find_dynamic(obj);
    if (!dyn)
        return 0;

    apply_relr(obj);

    size_t mbv = map_base_vaddr(obj);
    char *base = obj->base_addr;

    Elf64_Rela *rela = NULL;
    size_t rela_size = 0;
    Elf64_Rela *jmp_rela = NULL;
    size_t jmp_size = 0;

    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA:
            rela = (Elf64_Rela *)va(obj, d->d_un.d_ptr);
            break;
        case DT_RELASZ:
            rela_size = d->d_un.d_val;
            break;
        case DT_JMPREL:
            jmp_rela = (Elf64_Rela *)va(obj, d->d_un.d_ptr);
            break;
        case DT_PLTRELSZ:
            jmp_size = d->d_un.d_val;
            break;
        default:
            break;
        }
    }

    obj->jmp_rela = jmp_rela;
    obj->jmp_size = jmp_size;
    if (lazy_binding) {
        lazy_current = obj;
        if (lazy_obj_count < MAX_LAZY_OBJS)
            lazy_objs[lazy_obj_count++] = obj;
    }

    int count = 0;
    for (size_t off = 0; off < rela_size; off += sizeof(Elf64_Rela)) {
        Elf64_Rela *r = (Elf64_Rela *)((char *)rela + off);
        uint64_t *where = (uint64_t *)(base + (r->r_offset - mbv));
        size_t sym_idx = ELF64_R_SYM(r->r_info);
        switch (ELF64_R_TYPE(r->r_info)) {
        case R_AARCH64_RELATIVE:
            *where = (uint64_t)va(obj, r->r_addend);
            count++;
            break;
        case R_AARCH64_IRELATIVE:
            break;
        case R_AARCH64_TLS_TPREL:
        case R_AARCH64_TLSDESC:
            relocate_tls(obj, r, where);
            count++;
            break;
        case R_AARCH64_ABS64:
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT: {
            void *addr = NULL;
            if (sym_idx < obj->dynsym_count) {
                const Elf64_Sym *s = &obj->dynsym[sym_idx];
                const char *name = obj->dynstr + s->st_name;
                if (s->st_shndx == SHN_UNDEF)
                    addr = elf_resolve_import(obj, name);
                else
                    addr = va(obj, s->st_value);
            }
            if (addr) {
                if (ELF64_R_TYPE(r->r_info) == R_AARCH64_ABS64)
                    *where = (uint64_t)addr + r->r_addend;
                else
                    *where = (uint64_t)addr;
                count++;
            } else {
                /* GLOB_DAT/ABS64: loguj co se nuluje (kromě weak), ať nezůstane
                 * skrytý NULL slot (např. __stack_chk_guard → crash na ldr [x3]). */
                if (sym_idx < obj->dynsym_count &&
                    ELF64_ST_BIND(obj->dynsym[sym_idx].st_info) != STB_WEAK) {
                    fprintf(stderr, "[WARN] Unresolved RELA %s: %s in %s\n",
                        ELF64_R_TYPE(r->r_info) == R_AARCH64_GLOB_DAT ? "GLOB_DAT" :
                        ELF64_R_TYPE(r->r_info) == R_AARCH64_ABS64 ? "ABS64" : "JUMP_SLOT",
                        obj->dynstr + obj->dynsym[sym_idx].st_name,
                        obj->soname ? obj->soname : "EXE");
                    fflush(stderr);
                }
                *where = 0;
            }
            break;
        }
        default:
            break;
        }
    }

    for (size_t off = 0; jmp_rela && off < jmp_size; off += sizeof(Elf64_Rela)) {
        Elf64_Rela *r = (Elf64_Rela *)((char *)jmp_rela + off);
        uint64_t *where = (uint64_t *)(base + (r->r_offset - mbv));

        if (ELF64_R_TYPE(r->r_info) == R_AARCH64_IRELATIVE)
            continue;
        if (ELF64_R_TYPE(r->r_info) == R_AARCH64_TLSDESC ||
            ELF64_R_TYPE(r->r_info) == R_AARCH64_TLS_TPREL) {
            relocate_tls(obj, r, where);
            count++;
            continue;
        }
        if (obj->soname && strcmp(obj->soname, "libc.so.6") == 0 && ELF64_R_SYM(r->r_info) < obj->dynsym_count) {
            /* no-op */
        }
        if (lazy_binding) {
            *where = (uint64_t)lazy_plt_stub;
            count++;
            continue;
        }
        void *addr = resolve_jmp_symbol(obj, r);
        if (addr) {
            *where = (uint64_t)addr;
            count++;
        } else {
            /* weak undefined (__gmon_start__, __cxa_finalize...) je normalni -
             * tiskneme jen non-weak a jen pod ELF_DEBUG */
            const Elf64_Sym *ws =
                ELF64_R_SYM(r->r_info) < obj->dynsym_count
                    ? &obj->dynsym[ELF64_R_SYM(r->r_info)] : NULL;
            int is_weak = ws && ELF64_ST_BIND(ws->st_info) == STB_WEAK;
            *where = 0;
            if (!is_weak && elf_debug()) {
                fprintf(stderr, "[WARN] Unresolved JUMP_SLOT: %s in %s\n",
                        ws ? obj->dynstr + ws->st_name : "?",
                        obj->soname ? obj->soname : "EXE");
                fflush(stderr);
            }
        }
    }

    apply_segment_prots(obj);

    for (size_t off = 0; off < rela_size; off += sizeof(Elf64_Rela)) {
        Elf64_Rela *r = (Elf64_Rela *)((char *)rela + off);
        if (ELF64_R_TYPE(r->r_info) != R_AARCH64_IRELATIVE)
            continue;
        uint64_t *where = (uint64_t *)(base + (r->r_offset - mbv));
        void *res = call_ifunc_resolver(va(obj, r->r_addend));
        if (getenv("ELF_LOADER_DBG_IREL"))
            printf("[irel] %s addend=%#lx off=%#lx -> %p\n", obj->soname, (unsigned long)r->r_addend, (unsigned long)r->r_offset, res);
        *where = (uint64_t)res;
        count++;
    }
    for (size_t off = 0; jmp_rela && off < jmp_size; off += sizeof(Elf64_Rela)) {
        Elf64_Rela *r = (Elf64_Rela *)((char *)jmp_rela + off);
        if (ELF64_R_TYPE(r->r_info) != R_AARCH64_IRELATIVE)
            continue;
        uint64_t *where = (uint64_t *)(base + (r->r_offset - mbv));
        void *res2 = call_ifunc_resolver(va(obj, r->r_addend));
        if (getenv("ELF_LOADER_DBG_IREL"))
            printf("[irel.plt] %s addend=%#lx off=%#lx -> %p\n", obj->soname, (unsigned long)r->r_addend, (unsigned long)r->r_offset, res2);
        *where = (uint64_t)res2;
        count++;
    }

    if (elf_debug())
        printf("[+] relocated %d entries\n", count);
    obj->relocated = 1;
    return 0;
}

static void maybe_fixup_bionic_phdr(elf_object_t *obj) {
    char *base = obj->base_addr;
    int is_bionic = 0;

    for (int i = 0; i < obj->phdr_count; i++) {
        if (obj->phdr[i].p_type != PT_NOTE)
            continue;
        if (obj->phdr[i].p_filesz < 16)
            continue;
        const char *note = (const char *)va(obj, obj->phdr[i].p_vaddr);
        uint32_t n_namesz = *(const uint32_t *)note;
        if (n_namesz == 8 && memcmp(note + 12, "Android", 8) == 0) {
            is_bionic = 1;
            break;
        }
    }

    if (is_bionic) {
        Elf64_Phdr *mapped = (Elf64_Phdr *)(base + obj->ehdr->e_phoff);
        for (int i = 0; i < obj->phdr_count; i++)
            mapped[i].p_vaddr += (uint64_t)base;
        if (elf_debug())
            fprintf(stderr, "[dbg] bionic phdr p_vaddr rebased (+%p)\n", base);
    }
}

static void apply_segment_prots(elf_object_t *obj) {
    size_t mbv = map_base_vaddr(obj);
    char *base = obj->base_addr;
    for (int i = 0; i < obj->phdr_count; i++) {
        if (obj->phdr[i].p_type != PT_LOAD)
            continue;
        int prot = PROT_READ;
        if (obj->phdr[i].p_flags & PF_W)
            prot |= PROT_WRITE;
        if (obj->phdr[i].p_flags & PF_X)
            prot |= PROT_EXEC;
        size_t a = ALIGN_DOWN(obj->phdr[i].p_vaddr, PAGE_SIZE);
        size_t b = ALIGN_UP(obj->phdr[i].p_vaddr + obj->phdr[i].p_memsz, PAGE_SIZE);
        mprotect((char *)base + (a - mbv), b - a, prot);
    }
}

uintptr_t g_tls_new_tp = 0;
uintptr_t g_tls_old_tp = 0;

extern void jump_to_entry(void *entry, void *rsp, uintptr_t new_tp,
                          uintptr_t old_tp);

#define TLS_EXE_BASE_OFF 0x10u
#define TLS_PRE_TCB_SIZE 0x720u
#define TLS_TCB_HEAD_SIZE 0x800u

elf_tls_ctx_t elf_setup_own_tls(elf_object_t *exe, elf_scope_t *scope) {
    elf_tls_ctx_t ctx = {0};
    uintptr_t host_tp = read_tp();
    ctx.old_tp = host_tp;

    int64_t min_off = 0;
    size_t max_end = 0;
    if (exe && exe->has_tls) {
        min_off = TLS_EXE_BASE_OFF;
        max_end = (size_t)(TLS_EXE_BASE_OFF + exe->tls_memsz);
    }
    for (size_t i = 0; scope && i < scope->count; i++) {
        elf_object_t *m = scope->mods[i];
        if (m && m->has_tls) {
            int64_t off = (int64_t)m->tls_offset;
            if (off < min_off)
                min_off = off;
            size_t end = (size_t)(off + (int64_t)m->tls_memsz);
            if (end > max_end)
                max_end = end;
        }
    }
    /* FIX (btop/apt crash): i kdyz exe ani moduly nemaji PT_TLS, MUSIME vzdy
     * alokovat vlastni region s pthread struct headroomem a prepnout TP.
     * Modulove init_array (libstdc++ atd.) bezi jeste PRED trampolinou - pod
     * bionickym TPIDR_EL0 narazi parrot libc pri prvnim dotku pod TP-0x720
     * (_pthread_cleanup_push: cleanup listy, cancellable futex path) na guard
     * page [anon:stack_and_tls] -> SIGSEGV. Nulovana pthread struct v regionu
     * je validni prazdny stav (cleanup list head = NULL). */
    size_t span = max_end - (size_t)min_off;
    size_t size = ALIGN_UP(TLS_PRE_TCB_SIZE + span + 0x1000, PAGE_SIZE);
    void *region = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        return ctx;
    memset(region, 0, size);

    uintptr_t new_tp = (uintptr_t)region + TLS_PRE_TCB_SIZE + (size_t)(-min_off);
    /* region was zeroed above: struct pthread occupies [region, new_tp).
     * Pozn.: malloc thread_arena slot (TP-offset z libc .data @0x1afd68) zůstává
     * NULL = "uninitialized" -> glibc malloc si sám vezme main_arena. */

    /* tcbhead_t at new_tp: { dtv, private } -- dtv filled below */
    *(uintptr_t *)(new_tp + 0x00) = 0;
    *(uintptr_t *)(new_tp + 0x08) = 0;

    for (size_t i = 0; scope && i < scope->count; i++) {
        elf_object_t *m = scope->mods[i];
        if (!(m && m->has_tls))
            continue;
        /* .tdata image z modulu (arena/locale pointery libc!), ne garbage
         * z host TP (na Androidu bionic layout nekompatibilní). */
        char *dst = (char *)new_tp + m->tls_offset;
        if (m->tdata_src)
            memcpy(dst, m->tdata_src, m->tls_memsz);
        else
            memcpy(dst, (char *)host_tp + m->tls_offset, m->tls_memsz);
    }
    if (exe && exe->has_tls) {
        for (int i = 0; i < exe->phdr_count; i++) {
            if (exe->phdr[i].p_type != PT_TLS)
                continue;
            char *src = (char *)exe->base_addr +
                        (exe->phdr[i].p_vaddr - map_base_vaddr(exe));
            memcpy((char *)new_tp + TLS_EXE_BASE_OFF, src, exe->phdr[i].p_filesz);
            break;
        }
    }

    /* Build a glibc-shaped DTV so __tls_get_addr (GD/LD TLS) works.
       dtv_t layout: u[0]=counter | {val,to_free}, 16 bytes per entry. */
    typedef struct { uintptr_t u[2]; } ldso_dtv_t;
    size_t tls_mods = (exe && exe->has_tls) ? 1 : 0;
    for (size_t i = 0; scope && i < scope->count; i++)
        if (scope->mods[i] && scope->mods[i]->has_tls)
            tls_mods++;
    ldso_dtv_t *dtv = (ldso_dtv_t *)(new_tp + max_end + 0x800);
    memset(dtv, 0, sizeof(ldso_dtv_t) * (tls_mods + 2));
    dtv[0].u[0] = tls_mods + 1;          /* dtv length */
    dtv[1].u[0] = 1;                     /* TLS generation counter */
    size_t slot = 2;
    if (exe && exe->has_tls)
        dtv[slot++].u[0] = new_tp + TLS_EXE_BASE_OFF;
    for (size_t i = 0; scope && i < scope->count; i++) {
        elf_object_t *m = scope->mods[i];
        if (m && m->has_tls)
            dtv[slot++].u[0] = new_tp + m->tls_offset;
    }
    *(uintptr_t *)new_tp = (uintptr_t)&dtv[1]; /* tcbhead.dtv -> generation slot */

    g_tls_new_tp = new_tp;
    g_tls_old_tp = host_tp;
    ctx.region = region;
    ctx.size = size;
    /* POZOR: tady NESMÍ být msr tpidr_el0! Host bionic malloc (scudo) čte
     * per-thread cache z TLS pres TPIDR_EL0 - po switchi by kazdy loaderuv
     * malloc/free dereferencoval nulovy cache v parrot regionu -> SIGSEGV.
     * Switch dela az elf_run_final() tesne pred entry. */
    return ctx;
}

void elf_teardown_own_tls(elf_tls_ctx_t *ctx) {
    if (!ctx || !ctx->region)
        return;
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(ctx->old_tp));
    munmap(ctx->region, ctx->size);
    ctx->region = NULL;
    ctx->size = 0;
}

static Elf64_auxv_t *auxv_append(Elf64_auxv_t *a, uint64_t type, uint64_t val) {
    a->a_type = type;
    a->a_un.a_val = val;
    return a + 1;
}

static long sys_read(int fd, void *buf, size_t n) {
    register long x8 __asm__("x8") = 63;
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
    return x0;
}

static void sys_write(int fd, const void *buf, size_t n) {
    register long x8 __asm__("x8") = 64;
    register long x0 __asm__("x0") = fd;
    register const char *x1 __asm__("x1") = buf;
    register long x2 __asm__("x2") = (long)n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
}

#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif

/* Android app seccomp profil zabíjí TRAPem (SIGSYS) nove syscalls ktere
 * jadro 4.14 nema (clone3/close_range/openat2/faccessat2). Glibc 2.41 je
 * pouziva s fallbackem na stare varianty — ale fallback nikdy nepobezi,
 * protoze filtr misto ENOSYS da TRAP. Stacked filtr (bezi pred app
 * profilem) prelozi tyto cisla na ENOSYS -> glibc fallbacky zacnou fungovat.
 * Filtr se dedi pres fork+exec, takze kryje i spoustene binarky. */
void elf_install_compat(void);  /* see below */

static void install_legacy_syscall_filter_impl(void);
void elf_install_compat(void) {
    install_legacy_syscall_filter_impl();
}
static void install_legacy_syscall_filter_impl(void) {
    /* MINIMALNI program nejdrive — izolace EINVAL priciny */
    struct sock_filter prog[16];
    size_t n = 0;
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                            offsetof(struct seccomp_data, nr));
    /* aarch64 nr: clone3=435 close_range=436 openat2=437 faccessat2=439 */
    static const int blocked[] = { 435, 436, 437, 439 };
    for (size_t i = 0; i < sizeof(blocked)/sizeof(blocked[0]); i++) {
        prog[n++] = (struct sock_filter)
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, blocked[i], 0, 1);
        prog[n++] = (struct sock_filter)
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
    }
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    struct sock_fprog fprog = { .len = (unsigned short)n, .filter = prog };
    long pr = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    long sc = syscall((long)277 /* __NR_seccomp aarch64 */,
                      1UL /* SECCOMP_SET_MODE_FILTER */, 0UL, &fprog);
    if (sc != 0) {
        errno = 0;
        sc = prctl(PR_SET_SECCOMP /* 22 */, 1UL /* MODE_FILTER */, &fprog);
    }
    if (sc != 0) {
        char db[96]; char *dp = db;
        const char *pr = "[compat] filter install failed sc=";
        for (const char *q = pr; *q; q++) *dp++ = *q;
        unsigned long v = (unsigned long)(-sc);
        static const char hx[] = "0123456789abcdef";
        *dp++ = '-';
        for (int sh = 28; sh >= 0; sh -= 4) *dp++ = hx[(v >> sh) & 0xf];
        *dp++ = ' '; *dp++ = 'e'; *dp++ = 'r'; *dp++ = 'r'; *dp++ = 'n';
        *dp++ = 'o'; *dp++ = '=';
        v = (unsigned long)errno;
        for (int sh = 28; sh >= 0; sh -= 4) *dp++ = hx[(v >> sh) & 0xf];
        *dp++ = '\n';
        sys_write(2, db, (size_t)(dp - db));
    }
    (void)pr;
}

/* Mapuje adresu na soname+offset (+ nejbližší dynsym) přes g_crash_scope. */
static void elf_fault_map_one(uintptr_t addr) {
    if (!g_crash_scope || addr < 0x1000)
        return;
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < g_crash_scope->count; i++) {
        elf_object_t *m = g_crash_scope->mods[i];
        if (!m || !m->base_addr)
            continue;
        uintptr_t b = (uintptr_t)m->base_addr;
        if (addr >= b && addr < b + m->total_size) {
            uintptr_t off = addr - b;
            char b2[256]; char *p = b2;
            const char *q = "  @"; for (; *q; q++) *p++ = *q;
            const char *nm = m->soname ? m->soname : "EXE";
            for (; *nm; nm++) *p++ = *nm;
            *p++ = '+'; *p++ = '0'; *p++ = 'x';
            for (int s = 60; s >= 0; s -= 4) *p++ = hx[(off >> s) & 0xf];
            const char *sym = NULL; uintptr_t symoff = 0;
            if (m->dynsym && m->dynstr) {
                for (size_t j = 0; j < m->dynsym_count; j++) {
                    const Elf64_Sym *s = &m->dynsym[j];
                    if (s->st_shndx == SHN_UNDEF || s->st_name == 0)
                        continue;
                    if (s->st_value <= off && s->st_value > symoff) {
                        symoff = s->st_value;
                        sym = m->dynstr + s->st_name;
                    }
                }
            }
            if (sym) {
                *p++ = ' '; *p++ = '(';
                for (; *sym; sym++) *p++ = *sym;
                *p++ = '+'; *p++ = '0'; *p++ = 'x';
                for (int s = 60; s >= 0; s -= 4) *p++ = hx[((off - symoff) >> s) & 0xf];
                *p++ = ')';
            }
            *p++ = '\n';
            sys_write(2, b2, (size_t)(p - b2));
            return;
        }
    }
}

static void fault_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;

    static const char hexd[] = "0123456789abcdef";
    static char raw[2048];
    char *rp = raw;
    char *rpend = raw + sizeof(raw);
    #define RAW(c) do { if (rp < rpend-2) *rp++ = (c); } while (0)
    #define HX(vv) do { uintptr_t __v=(uintptr_t)(vv); for(int __sh=60;__sh>=0;__sh-=4) RAW(hexd[(__v>>__sh)&0xf]); } while (0)
    RAW('F'); RAW(':');
    uintptr_t cur_tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(cur_tp));
    RAW('t');RAW('p');RAW('='); HX(cur_tp); RAW(' ');
    RAW('p');RAW('c');RAW('='); HX(uc->uc_mcontext.pc); RAW(' ');
    RAW('s');RAW('p');RAW('='); HX(uc->uc_mcontext.sp); RAW(' ');
    RAW('a');RAW('d');RAW('='); HX(si->si_addr); RAW(' ');
    for (int i = 0; i <= 30; i++) {
        RAW('x'); RAW('0'+i/10); RAW('0'+i%10); RAW('=');
        HX(uc->uc_mcontext.regs[i]); RAW(' ');
    }
    RAW('\n');
    #undef RAW
    #undef HX
    {
        register long x8 __asm__("x8") = 56;  /* openat */
        register long x0 __asm__("x0") = AT_FDCWD;
        register const char *x1 __asm__("x1") = "/proc/self/maps";
        register long x2 __asm__("x2") = O_RDONLY;
        register long x3 __asm__("x3") = 0;
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                         : "memory", "cc");
        if (x0 >= 0) {
            int mfd = (int)x0;
            sys_write(2, "  [maps-begin]\n", 15);
            char mline[1024];
            long nr;
            while ((nr = sys_read(mfd, mline, sizeof mline)) > 0)
                sys_write(2, mline, (size_t)nr);
            sys_write(2, "  [maps-end]\n", 13);
        } else {
            sys_write(2, "  [openat-failed]\n", 18);
        }
    }

    {
        register long x8 __asm__("x8") = 64;
        register long x0 __asm__("x0") = 2;
        register const char *x1 __asm__("x1") = raw;
        register long x2 __asm__("x2") = (long)(rp - raw);
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                         : "memory", "cc");
    }
    elf_fault_map_one(uc->uc_mcontext.pc);
    elf_fault_map_one(uc->uc_mcontext.regs[30]); /* lr */

    /* Dumpy musejí jit pres sys_write: fprintf (bionic stdio) pod parrot TP
     * sam spadne (bionic cte pthread self pres x18 -> guard page) a buffered
     * vystup se pri smrti procesu nikdy nevyflushuje. */
    {
        char dbuf[256];
        #define DHX(vv) do { uintptr_t __v=(uintptr_t)(vv); \
            for(int __sh=60;__sh>=0;__sh-=4) { if(dp<dbuf+sizeof(dbuf)-1)*dp++=hexd[(__v>>__sh)&0xf]; } } while (0)
        #define DFLUSH() do { sys_write(2, dbuf, (size_t)(dp-dbuf)); dp = dbuf; } while (0)
        char *dp = dbuf;

        volatile uint32_t *iptr = (volatile uint32_t *)uc->uc_mcontext.pc;
        if ((uintptr_t)uc->uc_mcontext.pc > 0x10000) {
            for (int i = -2; i <= 2; i++) {
                uint32_t insn = *iptr;
                memcpy(dp, "  insn@", 6); dp += 6;
                DHX(iptr);
                memcpy(dp, "=", 1); dp += 1;
                DHX(insn);
                memcpy(dp, "\n", 1); dp += 1;
                DFLUSH();
                iptr++;
            }
        }

        memcpy(dp, "  stack:\n", 8); dp += 8; DFLUSH();
        {
            uintptr_t *s = (uintptr_t *)uc->uc_mcontext.sp;
            uintptr_t *smax = s + 64;
            for (int i = 0; s < smax && dp + 20 < dbuf + sizeof(dbuf); s++, i++) {
                if (i % 4 == 0) {
                    DHX(s);
                    memcpy(dp, ":", 1); dp += 1;
                }
                memcpy(dp, " ", 1); dp += 1;
                DHX(*s);
                if (i % 4 == 3)
                    memcpy(dp, "\n", 1), dp += 1;
            }
            memcpy(dp, "\n", 1); dp += 1;
            DFLUSH();
        }

        {
            uintptr_t *fp = (uintptr_t *)uc->uc_mcontext.regs[29];
            for (int i = 0; i < 12 && fp && (uintptr_t)fp > 0x1000; i++) {
                uintptr_t *next = (uintptr_t *)*fp;
                uintptr_t ra = fp[1];
                memcpy(dp, "  frame ra=", 11); dp += 11;
                DHX(ra);
                memcpy(dp, "\n", 1); dp += 1;
                DFLUSH();
                if (next <= fp || (uintptr_t)next > 0x7fffffffffffUL)
                    break;
                fp = next;
            }
        }
        #undef DHX
        #undef DFLUSH
    }

    Dl_info di;
    if (dladdr((void *)uc->uc_mcontext.pc, &di) && di.dli_fname)
        fprintf(stderr, "  pc in: %s (%s+%#lx)\n", di.dli_fname,
                di.dli_sname ? di.dli_sname : "?",
                (unsigned long)((char *)uc->uc_mcontext.pc -
                                (char *)di.dli_fbase));
    fflush(stderr);
    _exit(128 + sig);
}


/* SIGSYS = seccomp odmitl syscall. Vypise cislo syscallu cistym sys_write
 * (handler muze bezet pod parrot TP, bionic stdio je tam nedostupne). */
static void sigsys_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    char buf[64];
    const char *pre = "[SIGSYS] denied syscall nr=";
    char *p = buf;
    for (const char *q = pre; *q; q++) *p++ = *q;
    long nr = si->si_syscall;
    char tmp[24]; int ti = 0;
    if (nr == 0) tmp[ti++] = '0';
    while (nr > 0) { tmp[ti++] = (char)('0' + (nr % 10)); nr /= 10; }
    while (ti > 0) *p++ = tmp[--ti];
    *p++ = '\n';
    sys_write(2, buf, (size_t)(p - buf));
    _exit(159);
}

void elf_install_fault_handlers(void) {
    static char altstack[32768];
    static stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, NULL);
    /* SIGSYS: vypise cislo odmitaneho syscallu a exit(159) */
    struct sigaction sc;
    memset(&sc, 0, sizeof(sc));
    sc.sa_sigaction = sigsys_handler;
    sc.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSYS, &sc, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* DEBUG (ELF_LOADER_DEBUG_PC): zablokuj rt_sigaction(SIGSEGV) -> EPERM,
 * aby target nemohl prepsat loaderuv fault handler a my videli skutecny
 * PC jeho crashi (napr. procps instaluje vlastni SIGSEGV handler). */
static void elf_install_debug_sigaction_block(void) {
    struct sock_filter prog[8];
    size_t n = 0;
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                            offsetof(struct seccomp_data, nr));
    prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                            134 /* rt_sigaction aarch64 */, 0, 4);
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                            offsetof(struct seccomp_data, args[0]));
    prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 11 /* SIGSEGV */, 0, 1);
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    struct sock_fprog fprog = { .len = (unsigned short)n, .filter = prog };
    long r = syscall((long)277, 1UL, 0UL, &fprog);
    { char b[48]; char *p = b; const char *q = "[dbg-sa-block] ret=";
      for (; *q; q++) *p++ = *q;
      long v = r; if (v < 0) v = -v;
      static const char hx[] = "0123456789abcdef";
      for (int s = 28; s >= 0; s -= 4) *p++ = hx[(v >> s) & 0xf];
      *p++ = '\n'; sys_write(2, b, (size_t)(p - b)); }
}

int elf_run(elf_object_t *obj, int argc, char **argv, char **envp) {
    if (!obj)
        return -1;

    apply_segment_prots(obj);

    size_t env_count = 0;
    while (envp[env_count])
        env_count++;

    size_t stack_size = 8 * 1024 * 1024;
    char *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        perror("mmap stack");
        return -1;
    }

    char *stack_top = stack + stack_size;

    size_t str_total = 256 + argc * 128 + env_count * 256;

    size_t argv_off_size = (argc > 0) ? argc : 1;
    size_t envp_off_size = (env_count > 0) ? env_count : 1;
    size_t *argv_off = calloc(argv_off_size, sizeof(size_t));
    size_t *envp_off = calloc(envp_off_size, sizeof(size_t));
    if (!argv_off || !envp_off)
        return -1;

    size_t frame = 8 + (argc + 1) * 8 + (env_count + 1) * 8 + 24 * 2 * 8 + 16 + str_total;
    char *sp = stack_top - frame;
    sp = (char *)((uintptr_t)sp & ~(uintptr_t)15);

    uint64_t *argc_slot = (uint64_t *)sp;
    uint64_t *argv_arr = argc_slot + 1;
    uint64_t *envp_arr = argv_arr + (argc + 1);
    Elf64_auxv_t *aux = (Elf64_auxv_t *)(envp_arr + env_count + 1);
    uint8_t *rand_bytes = (uint8_t *)(aux + 24);
    char *strings = (char *)(rand_bytes + 16);

    size_t off = 0;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        argv_off[i] = off;
        memcpy(strings + off, argv[i], len);
        off += len;
    }
    for (size_t i = 0; i < env_count; i++) {
        size_t len = strlen(envp[i]) + 1;
        envp_off[i] = off;
        memcpy(strings + off, envp[i], len);
        off += len;
    }

    *argc_slot = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        argv_arr[i] = (uint64_t)(strings + argv_off[i]);
    argv_arr[argc] = 0;
    free(argv_off);

    for (size_t i = 0; i < env_count; i++)
        envp_arr[i] = (uint64_t)(strings + envp_off[i]);
    envp_arr[env_count] = 0;
    free(envp_off);

    Elf64_auxv_t *a = aux;
    a = auxv_append(a, AT_PHDR, (uint64_t)((char *)obj->base_addr + obj->ehdr->e_phoff));
    a = auxv_append(a, AT_PHENT, sizeof(Elf64_Phdr));
    a = auxv_append(a, AT_PHNUM, obj->phdr_count);
    a = auxv_append(a, AT_PAGESZ, (uint64_t)sysconf(_SC_PAGESIZE));
    a = auxv_append(a, AT_ENTRY, (uint64_t)obj->entry_point);
    a = auxv_append(a, AT_BASE, (uint64_t)obj->base_addr);
    a = auxv_append(a, AT_UID, (uint64_t)getuid());
    a = auxv_append(a, AT_GID, (uint64_t)getgid());
    a = auxv_append(a, AT_SECURE, 0);
    a = auxv_append(a, AT_RANDOM, (uint64_t)rand_bytes);
    a = auxv_append(a, AT_HWCAP, (uint64_t)getauxval(AT_HWCAP));
    a = auxv_append(a, AT_HWCAP2, (uint64_t)getauxval(AT_HWCAP2));
    a = auxv_append(a, AT_EXECFN, (uint64_t)(argv[0] ? (uintptr_t)argv[0] : 0));
    a = auxv_append(a, AT_NULL, 0);
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            ssize_t got = read(fd, rand_bytes, 16);
            if (got < 16)
                memset(rand_bytes, 0x5a, 16);
            close(fd);
        } else {
            memset(rand_bytes, 0x5a, 16);
        }
    }

    elf_install_fault_handlers();

    if (elf_debug())
        printf("[+] entering %p (stack %p) tp=%p inits=%zu\n", obj->entry_point, sp,
           (void *)g_tls_new_tp, g_pending_count);
    fflush(stdout);

    /* Finální fáze: od tady už ŽÁDNÝ bionic kód (žádný malloc/stdio).
     * 1) switch na parrot TP (region má nulovanou pthread struct -> cleanup
     *    listy/mutexy jsou validní prázdné),
     * 2) spusit queued DT_INIT + init_array všech modulů POD parrot TP,
     * 3) skoč na exe entry s exe stackem. Nikdy se nevrací (exit_group). */
    elf_run_final(sp, obj->entry_point, obj);
    return -1;
}

/* Běží pod parrot TP; smí volat jen parrot kód a loaderovu pointer
 * aritmetiku. Inits můžou lazy-resolvovat importy - resolve path je po
 * úklidu debug printů malloc-free. */
/* Běží pod parrot TP; smí volat jen parrot kód a loaderovu pointer
 * aritmetiku. Inits můžou lazy-resolvovat importy - resolve path je po
 * úklidu debug printů malloc-free. */
static __attribute__((noreturn)) void elf_run_final(void *sp, void *entry,
                                                    elf_object_t *obj) {
    (void)obj;
    if (!g_tls_new_tp) {
        /* Staré flow (--run/--own/--shim): žádný TLS switch, inity pod host TP
         * (jak to dělal jump_to_entry). */
        for (size_t i = 0; i < g_pending_count; i++)
            g_pending_inits[i](elf_init_argc, elf_init_argv, elf_init_envp);
        jump_to_entry(entry, sp, 0, 0);
        __builtin_unreachable(); /* exe končí exit_group, sem se nedostane */
    }
    if (elf_debug())
        fprintf(stderr, "[dbg-final] tp=%p sp=%p entry=%p pending=%zu\n",
            (void *)g_tls_new_tp, sp, entry, g_pending_count);
    /* některé programy (procps ps/top) si instalují vlastní SIGSEGV
     * handler → náš fault dump se nezobrazí; flag umožní reinstalaci */
    if (getenv("ELF_LOADER_KEEP_HANDLERS"))
        elf_install_fault_handlers();
    if (getenv("ELF_LOADER_DEBUG_PC"))
        elf_install_debug_sigaction_block();
    fflush(stderr);
    extern void elf_final_jump(void *, void *, uintptr_t, void (*)(void));
    elf_final_jump(sp, entry, g_tls_new_tp, elf_run_pending_inits);
    __builtin_unreachable();
}

void elf_unload(elf_object_t *obj) {
    if (!obj)
        return;
    if (obj->base_addr)
        munmap(obj->base_addr, obj->total_size);
    free(obj->ehdr);
    free(obj->phdr);
    free(obj->symtab);
    free(obj->strtab);
    free(obj->dynsym);
    free(obj->dynstr);
    for (size_t i = 0; i < obj->handle_count; i++)
        if (obj->handles[i])
            dlclose(obj->handles[i]);
    free(obj->handles);
    free(obj->origin_dir);
    free(obj);
}