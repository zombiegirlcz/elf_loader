#define _GNU_SOURCE
#include "../include/elf_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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
#include <sys/wait.h>

/* SENTINEL v x5 (args[5]) pro seccomp path-filter: handlerova emulace raw
 * syscallu nese toto cislo v x5, filtr ji tak pousti (zabrani zacykleni).
 * Definovano zde, aby bylo dostupne i fault_handleru (maps dump). */
#define F2_SENTINEL 0x1234567890ABCDEFULL
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

/* ---- TLS resolution tracing (diagnoza: kam se resolvuje _dl_allocate_tls) ----
 * Vse pres raw write(2) syscall, aby to fungovalo i pod parrot TP (bionic
 * stdio pod guest TP pada). Zapina se env ELF_LOADER_TLS_TRACE=1. */
int g_tls_trace = 0;

static long tls_raw_write(int fd, const void *buf, size_t n) {
    register long x8 __asm__("x8") = 64;
    register long x0 __asm__("x0") = fd;
    register const void *x1 __asm__("x1") = buf;
    register long x2 __asm__("x2") = (long)n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
    return x0;
}

static int tls_name_match(const char *name) {
    return name && (strcmp(name, "_dl_allocate_tls") == 0 ||
                    strcmp(name, "_dl_allocate_tls_init") == 0 ||
                    strcmp(name, "_dl_deallocate_tls") == 0);
}

static void tls_trace(const char *name, void *addr, const char *via,
                      const char *objname) {
    if (!g_tls_trace || !tls_name_match(name))
        return;
    static const char hx[] = "0123456789abcdef";
    char buf[192];
    char *p = buf;
    char *end = buf + sizeof(buf) - 1;
    #define TP(s) do { const char *__s = (s); while (*__s && p < end) *p++ = *__s++; } while (0)
    #define TH(v) do { uintptr_t __v = (uintptr_t)(v); \
        TP("0x"); for (int __sh = 60; __sh >= 0; __sh -= 4) \
            { if (p < end) *p++ = hx[(__v >> __sh) & 0xf]; } } while (0)
    TP("[TLSRES] "); TP(name); TP(" via="); TP(via);
    TP(" -> "); TH(addr); TP(" obj="); TP(objname ? objname : "?");
    *p++ = '\n';
    tls_raw_write(2, buf, (size_t)(p - buf));
    #undef TP
    #undef TH
}

int elf_init_argc = 0;
char **elf_init_argv = NULL;
char **elf_init_envp = NULL;

/* ---- Staticky TLS registry (aarch64 TLS_DTV_AT_TP) --------------------
 * struct pthread je PRED thread pointerem (TP), tcbhead_t (16 B) na TP a
 * TLS bloky NAD nim (TP + offset). Kazdemu modulu s PT_TLS pridame maly
 * kladny offset od TP. pthread_create pak pres GLRO(dl_tls_static_size)
 * rezervuje prostor nad TP; ldso_tls.c naplni DTV a kopie .tdata. */
#define ELF_MAX_TLS_MODS 128
static elf_object_t *g_tls_mods[ELF_MAX_TLS_MODS];
static size_t g_tls_mod_count;
static uintptr_t g_tls_next = ELF_TLS_TCB_SIZE; /* TLS bloky od TP+0x10 (glibc TLS_TCB_SIZE) */
#define ELF_TLS_DTV_RESERVE 0x1000u

static void elf_tls_assign(elf_object_t *m, size_t align) {
    if (!m || !m->has_tls || g_tls_mod_count >= ELF_MAX_TLS_MODS)
        return;
    if (align == 0)
        align = 1;
    uintptr_t off = ALIGN_UP(g_tls_next, align);
    if (off + ALIGN_UP(m->tls_memsz, align) > ELF_TLS_RESERVE) {
        /* Rezerva pretelka. NESMIME nechat tls_offset=0 (has_tls=1) - pak by
         * elf_tls_add_module_to_thread zapsal .tdata na TP+0 a prepsal
         * tcbhead.dtv -> rozbil cely TLS. Modul bez TLS je mene zle. */
        fprintf(stderr, "[WARN] TLS reserve exhausted for %s\n",
                m->soname ? m->soname : "?");
        m->has_tls = 0;
        return;
    }
    m->tls_offset = off;
    g_tls_next = off + ALIGN_UP(m->tls_memsz, align);
    g_tls_mods[g_tls_mod_count++] = m;
}

size_t elf_tls_module_count(void) { return g_tls_mod_count; }
elf_object_t *elf_tls_module_at(size_t i) {
    return i < g_tls_mod_count ? g_tls_mods[i] : NULL;
}
uintptr_t elf_tls_span(void) { return g_tls_next; }
/* rseq area je na PEVNEM offsetu ELF_TLS_RESERVE (za rezervou pro TLS bloky
 * vsech modulu). Diky tomu dynamicky nacitane moduly (Python extension .so)
 * nikdy nekoliduji s rseq/DTV a region se nemusi realokovat za behu.
 * Hlavni exe (non-PIE, zapečene TPREL offsety) sedi na TP+0x10 = OK. */
uintptr_t elf_tls_rseq_offset(void) { return ELF_TLS_RESERVE; }
uintptr_t elf_tls_static_size(void) {
    return ALIGN_UP(elf_tls_rseq_offset() + ELF_RSEQ_SIZE, 16)
           + (2 + ELF_MAX_TLS_MODS + 16) * 16   /* DTV pro max modulu */
           + ELF_TLS_DTV_RESERVE;
}

const char *loader_phase = "start";
uintptr_t g_libc_base = 0;
uintptr_t g_exe_base = 0;

static int is_ld_linux(const char *name) {
    return strncmp(name, "ld-linux", 8) == 0 || strcmp(name, "ld.so.1") == 0;
}

#define SYSTEM_LIBDIRS "/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu" \
                       ":/usr/lib:/lib"

static char *derive_distro_libdirs(const char *origin_dir);
static const char *find_distro_root(const char *origin_dir);

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
static int g_debug_level = 0;

int elf_debug(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *d = getenv("ELF_DEBUG");
        cached = (d && d[0] && strcmp(d, "0") != 0) ? 1 : 0;
        g_debug_level = cached;
    }
    return cached;
}

void elf_debug_init(void) {
    elf_debug(); /* Initialize debug level from environment */
}

void elf_debug_set_level(int level) {
    g_debug_level = level;
}

int elf_debug_get_level(void) {
    return g_debug_level;
}

void elf_debug_log(const char *fmt, ...) {
    if (g_debug_level < ELF_DEBUG_LEVEL_INFO) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[dbg] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void elf_debug_trace(const char *fmt, ...) {
    if (g_debug_level < ELF_DEBUG_LEVEL_DEBUG) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[trc] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void elf_debug_dump_maps(void) {
    if (g_debug_level < ELF_DEBUG_LEVEL_DEBUG) return;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[256];
    fprintf(stderr, "[dbg] === /proc/self/maps ===\n");
    while (fgets(line, sizeof(line), f)) {
        fprintf(stderr, "[dbg] %s", line);
    }
    fclose(f);
    fprintf(stderr, "[dbg] =========================\n");
}

void elf_debug_dump_symbols(elf_object_t *obj) {
    if (g_debug_level < ELF_DEBUG_LEVEL_DEBUG) return;
    if (!obj || !obj->dynsym || !obj->dynstr) return;
    fprintf(stderr, "[dbg] Symbols for %s:\n", obj->soname ? obj->soname : "<unknown>");
    for (size_t i = 0; i < obj->dynsym_count; i++) {
        Elf64_Sym *sym = &obj->dynsym[i];
        const char *name = obj->dynstr + sym->st_name;
        const char *type = "?";
        switch (ELF64_ST_TYPE(sym->st_info)) {
            case STT_FUNC: type = "FUNC"; break;
            case STT_OBJECT: type = "OBJ"; break;
            case STT_SECTION: type = "SEC"; break;
            case STT_FILE: type = "FILE"; break;
            case STT_NOTYPE: type = "NOTYPE"; break;
        }
        fprintf(stderr, "[dbg]   %4zu: %s @ 0x%lx (%s)\n", i, name, (unsigned long)sym->st_value, type);
    }
}

void elf_debug_dump_relocations(elf_object_t *obj) {
    if (g_debug_level < ELF_DEBUG_LEVEL_DEBUG) return;
    if (!obj || !obj->jmp_rela) return;
    fprintf(stderr, "[dbg] JMP_RELA for %s:\n", obj->soname ? obj->soname : "<unknown>");
    for (size_t i = 0; i < obj->jmp_size / sizeof(Elf64_Rela); i++) {
        Elf64_Rela *rela = &obj->jmp_rela[i];
        fprintf(stderr, "[dbg]   [%zu] offset=0x%lx type=%lu sym=%lu addend=%ld\n",
                i, (unsigned long)rela->r_offset, 
                (unsigned long)ELF64_R_TYPE(rela->r_info), 
                (unsigned long)ELF64_R_SYM(rela->r_info), 
                (long)rela->r_addend);
    }
}

void elf_debug_dump_memory(void *addr, size_t len) {
    if (g_debug_level < ELF_DEBUG_LEVEL_VERBOSE) return;
    unsigned char *p = (unsigned char *)addr;
    fprintf(stderr, "[dbg] Memory dump at %p (%zu bytes):\n", addr, len);
    for (size_t i = 0; i < len; i += 16) {
        fprintf(stderr, "[dbg] %p: ", p + i);
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            fprintf(stderr, "%02x ", p[i + j]);
        }
        fprintf(stderr, "  ");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            char c = p[i + j];
            fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        fprintf(stderr, "\n");
    }
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
/* __rseq_offset MUSI ukazovat na nasu dedikovanou rseq area v TLS (TP+0x10),
 * ktera je naplnena 0xFF => cpu_id=-1 < 0 => glibc new-thread do_rseq=false
 * => nezavola rseq (293, app seccomp KILL). Kdyby offset byl 0, glibc by
 * prepsala DTV pointer na TP a stejne by registrovala. */
static int64_t ldso_rseq_offset = ELF_TLS_TCB_SIZE;
static unsigned int ldso_rseq_size = 0;
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
    /* ldso_register_linkmap je definovan nize; deklarace vpred. */
    extern size_t ldso_register_linkmap(elf_object_t *m);
    if (count > LDSO_MAX_MODULES)
        count = LDSO_MAX_MODULES;
    for (size_t i = 0; i < count; i++)
        (void)ldso_register_linkmap(mods[i]);
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

/* --- Bionic <-> parrot TLS prepinani pro shimy volane z guest glibc ---
 * Loaderuv kod je bionicky (scudo malloc, bionic libc) a MUSI bezet pod
 * bionickym TP. Guest glibc ale vola nase _rtld_global_ro shimy pod parrot
 * TP; kdyz shim zavola bionickou funkci (malloc/free/getenv/stat/snprintf),
 * bionic si pres tpidr_el0 precte parrot TLS jako sve struktury -> SIGSEGV.
 * (Presne to padalo u node/V8 __backtrace -> _dl_find_object / _dl_open
 * a u zsh/bash pri getpwuid -> dl_iterate_phdr.) */
extern uintptr_t g_tls_old_tp;

static inline uintptr_t dl_tp_get(void) {
    uintptr_t t; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(t)); return t;
}
static inline void dl_tp_set(uintptr_t t) {
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(t));
}
/* Vstoupí do loader (bionic) scope. Vrací 1 = na konci obnovit TP. */
static inline int dl_enter_host(uintptr_t *saved) {
    uintptr_t cur = dl_tp_get();
    *saved = cur;
    if (g_tls_old_tp && cur != g_tls_old_tp) {
        dl_tp_set(g_tls_old_tp);
        return 1;
    }
    return 0;
}
static inline void dl_leave_host(int sw, uintptr_t saved) {
    if (sw) dl_tp_set(saved);
}

/* glibc 2.41+ _dl_find_object: (const void *pc, struct dl_find_object *result).
   Fills result for the object containing PC, returns 0 on success / -1 on
   not-found.  Result layout: +0 dlfo_addr, +8 dlfo_name, +16 dlfo_phdr,
   +24 dlfo_phnum, +32 dlfo_map_start, +40 dlfo_map_end, +48 dlfo_link_map.  */
static int ldso_find_object_impl(uintptr_t pc, void *result) {
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

/* Wrapper: guest glibc vola _dl_find_object pod parrot TP, ale telo shimu
 * pouziva loaderuv bionicky kod -> prepni na bionic TP. */
static int ldso_find_object(uintptr_t pc, void *result) {
    uintptr_t _s; int _sw = dl_enter_host(&_s);
    int _r = ldso_find_object_impl(pc, result);
    dl_leave_host(_sw, _s);
    return _r;
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

/* Map an elf_object_t back to its fake link_map (built by
 * ldso_install_module_list / ldso_install_exe_linkmap). Falls back to the exe
 * link_map if the module is not in the list (e.g. the main executable). */
/* Zaregistruje linkmap pro modul (i dynamicky nacteny za behu pres dlopen).
 * Bez tohoto zaznamu v ldso_module_linkmaps vraci ldso_linkmap_for fallback
 * ldso_exe_linkmap, takze glibc pocita adresy symbolu proti spatnemu base ->
 * flaky SIGSEGV zavisly na ASLR layoutu. Dedup podle l_addr. */
size_t ldso_register_linkmap(elf_object_t *m) {
    if (!m)
        return (size_t)-1;
    uintptr_t base = (uintptr_t)m->base_addr;
    for (size_t i = 0; i < ldso_module_count; i++) {
        unsigned char *b = (unsigned char *)ldso_module_linkmaps[i];
        if (*(uintptr_t *)(b + 0x00) == base)
            return i;
    }
    if (ldso_module_count >= LDSO_MAX_MODULES)
        return (size_t)-1;
    uint64_t *lm = ldso_module_linkmaps[ldso_module_count];
    memset(lm, 0, sizeof ldso_module_linkmaps[0]);
    unsigned char *b = (unsigned char *)lm;
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
    return ldso_module_count++;
}

static void *ldso_linkmap_for(elf_object_t *m) {
    if (!m)
        return NULL;
    uintptr_t base = (uintptr_t)m->base_addr;
    if (*(uintptr_t *)((unsigned char *)ldso_exe_linkmap + 0x00) == base)
        return ldso_exe_linkmap;
    for (size_t i = 0; i < ldso_module_count; i++) {
        unsigned char *b = (unsigned char *)ldso_module_linkmaps[i];
        if (*(uintptr_t *)(b + 0x00) == base)
            return ldso_module_linkmaps[i];
    }
    /* Modul nacteny za behu (dlopen/NSS/ctypes/Go) - zaregistruj linkmap,
     * aby glibc dostal spravny l_addr. Jinak by vratil exe linkmap a pocital
     * adresy proti spatnemu base -> flaky crash dle ASLR layoutu. */
    {
        size_t idx = ldso_register_linkmap(m);
        if (idx != (size_t)-1)
            return ldso_module_linkmaps[idx];
    }
    return ldso_exe_linkmap;
}

/* glibc 2.41 _dl_lookup_symbol_x shim: resolve a symbol name in the guest
 * scope. Returns the defining object's link_map and sets *ref to the symbol
 * (or NULL when not found). glibc's dlsym()/dlopen() internals call this via
 * the _rtld_global_ro function table (offset 0x268); without it the call
 * goes through a NULL pointer -> SIGSEGV pc=0 (starship/Rust crash). */
static void *ldso_lookup_symbol_x_impl(const char *name, void *undef_map,
                                       const void **ref, void **scope,
                                       const void *version, int type_class,
                                       int flags, void *skip_map) {
    (void)undef_map; (void)scope; (void)version;
    (void)type_class; (void)flags; (void)skip_map;
    if (ref)
        *ref = NULL;
    if (!name || !g_crash_scope)
        return NULL;
    const Elf64_Sym *sym = NULL;
    elf_object_t *m = elf_scope_find(g_crash_scope, name, &sym);
    if (!m || !sym)
        return NULL;
    if (ref)
        *ref = sym;
    return ldso_linkmap_for(m);
}

/* Wrapper: bionicky loader kod (memset/strncpy v ldso_register_linkmap). */
static void *ldso_lookup_symbol_x(const char *name, void *undef_map,
                                  const void **ref, void **scope,
                                  const void *version, int type_class,
                                  int flags, void *skip_map) {
    uintptr_t _s; int _sw = dl_enter_host(&_s);
    void *_r = ldso_lookup_symbol_x_impl(name, undef_map, ref, scope,
                                         version, type_class, flags, skip_map);
    dl_leave_host(_sw, _s);
    return _r;
}

/* _rtld_global_ro function-table shims (glibc calls these instead of going
 * through the PLT).  Offsets follow the real glibc 2.41 layout. */
static void ldso_debug_printf(const char *fmt, ...) {
    uintptr_t _s; int _sw = dl_enter_host(&_s);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    dl_leave_host(_sw, _s);
}

static void ldso_mcount(uintptr_t frompc, uintptr_t selfpc) {
    (void)frompc; (void)selfpc;
}

static void ldso_dl_close(void *map) {
    (void)map;
}

static int ldso_catch_error(const char **objname, const char **errstring,
                            unsigned char *mallocedp,
                            void (*operate)(void *), void *args) {
    if (objname) *objname = NULL;
    if (errstring) *errstring = NULL;
    if (mallocedp) *mallocedp = 0;
    operate(args);
    return 0;
}

static void ldso_error_free(void *p) {
    uintptr_t _s; int _sw = dl_enter_host(&_s);
    free(p);
    dl_leave_host(_sw, _s);
}

static void ldso_libc_freeres(void) {
}

/* _dl_open: guest dlopen.  Resolve the path against ROOTFS and own-load the
 * .so into the guest scope, then return its (fake) link_map as the handle. */
/* Fronta init funkci je definovana nize (elf_queue_init / elf_run_pending_inits).
 * Runtime _dl_open ji ale take potrebuje spustit - jinak zustanou konstruktory
 * noveho modulu nezavolane (OpenSSL libcrypto: bez sveho OSSL ctoru nemá
 * registry provideru -> OSSL_PROVIDER_available("default")==0 -> node
 * CHECK(ncrypto::CSPRNG(nullptr,0)) assert). */
typedef void (*init_fn_t)(int, char **, char **);
static init_fn_t *g_pending_inits;
static size_t g_pending_count;
extern uintptr_t g_tls_new_tp;

static void *ldso_dl_open_impl(const char *file, int mode, const void *caller,
                               long nsid, int argc, char *argv[], char *env[]) {
    (void)mode; (void)caller; (void)nsid; (void)argc; (void)argv; (void)env;
    if (!file || !file[0] || !g_crash_scope)
        return NULL;
    char resolved[4096];
    const char *root = getenv("ROOTFS");
    size_t rl = root ? strlen(root) : 0;
    if (file[0] == '/') {
        if (rl && strncmp(file, root, rl) != 0)
            snprintf(resolved, sizeof resolved, "%s%s", root, file);
        else
            snprintf(resolved, sizeof resolved, "%s", file);
    } else {
        /* Soname bez cesty (libtinfo.so.6 z NEEDED zsh modulu). Hledej v
         * standardnich distro libdirs pod ROOTFS - jinak zsh zmodload
         * terminfo/termcap nenajde libtinfo a spadne. */
        resolved[0] = 0;
        if (rl) {
            static const char *sub[] = {
                "/usr/lib/aarch64-linux-gnu", "/lib/aarch64-linux-gnu",
                "/usr/lib", "/lib",
                "/usr/local/lib/aarch64-linux-gnu", "/usr/local/lib",
                NULL
            };
            for (int i = 0; sub[i]; i++) {
                char cand[4096];
                snprintf(cand, sizeof cand, "%s%s/%s", root, sub[i], file);
                struct stat _st;
                if (stat(cand, &_st) == 0) {
                    snprintf(resolved, sizeof resolved, "%s", cand);
                    break;
                }
            }
        }
        if (!resolved[0])
            snprintf(resolved, sizeof resolved, "%s", file);
    }
    size_t prev_count = g_pending_count;
    elf_object_t *m = elf_load_shared(resolved, g_crash_scope);
    /* Nove zarazene inity spust pod parrot TP - jsou to konstruktory guest
     * modulu (libcrypto OSSL ctor, libstdc++ atd.), ktere sahaji do guest TLS.
     * loader sam (a ldso_linkmap_for nize) musi zustat pod bionickym TP. */
    if (m && !getenv("ELF_LOADER_NO_INITS") && g_pending_count > prev_count) {
        uintptr_t save2 = dl_tp_get();
        if (g_tls_new_tp)
            dl_tp_set(g_tls_new_tp);
        for (size_t i = prev_count; i < g_pending_count; i++) {
            init_fn_t fn = g_pending_inits[i];
            if (fn)
                fn(elf_init_argc, elf_init_argv, elf_init_envp);
        }
        dl_tp_set(save2);
    }
    return ldso_linkmap_for(m);
}

/* Wrapper: elf_load_shared + ldso_linkmap_for jsou bionicky loader kod
 * (malloc/memset/stat/getenv). Guest glibc sem vstupuje pod parrot TP. */
static void *ldso_dl_open(const char *file, int mode, const void *caller,
                          long nsid, int argc, char *argv[], char *env[]) {
    uintptr_t _s; int _sw = dl_enter_host(&_s);
    void *_r = ldso_dl_open_impl(file, mode, caller, nsid, argc, argv, env);
    dl_leave_host(_sw, _s);
    return _r;
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
    /* _rtld_global_ro function table (used by libc internals instead of PLT).
     * Offsets are the REAL glibc 2.41 layout: the table starts at 0x258.
     *   +0x258 _dl_debug_printf, +0x260 _dl_mcount,
     *   +0x268 _dl_lookup_symbol_x, +0x270 _dl_open, +0x278 _dl_close,
     *   +0x280 _dl_catch_error, +0x288 _dl_error_free,
     *   +0x290 _dl_tls_get_addr_soft, +0x298 _dl_libc_freeres,
     *   +0x2a0 _dl_find_object. */
    /* TLS static layout.  allocate_stack reads GLRO(dl_tls_static_align)
     * (offset 0x1d0+8) and asserts size != 0 after masking; with align=0 the
     * mask becomes ~0xffff.. and size collapses to 0 (starship/Rust crash). */
    /* _dl_tls_static_size (_rtld_global_ro+0x1d8): MUSI byt nenulove.
     * pthread_create/allocate_stack pocita pro novy thread:
     *   TP = (mmap_top - tls_static_size_for_stack) & -align
     * a pro aarch64 (TLS_DTV_AT_TP) rostou TLS bloky NAD TP. Kdyz je
     * static_size == 0, TP == mmap_top == prvni bajt za regionem (casto
     * sousedi s libc) -> zapis v _dl_allocate_tls faultuje.
     * Hodnotu lze prebit env pro ladeni (ELF_LOADER_TLS_SIZE). */
    {
        const char *tss = getenv("ELF_LOADER_TLS_SIZE");
        unsigned long tls_size = (tss && tss[0])
                                     ? strtoul(tss, NULL, 0)
                                     : (unsigned long)elf_tls_static_size();
        ro[0x1d8 / 8] = tls_size; /* _dl_tls_static_size */
    }
    ro[0x1e0 / 8] = 16;               /* _dl_tls_static_align */
    ro[0x1e8 / 8] = 0;                /* _dl_tls_static_surplus */

    ro[0x258 / 8] = (uintptr_t)ldso_debug_printf;    /* _dl_debug_printf */
    ro[0x260 / 8] = (uintptr_t)ldso_mcount;           /* _dl_mcount */
    ro[0x268 / 8] = (uintptr_t)ldso_lookup_symbol_x;  /* _dl_lookup_symbol_x */
    ro[0x270 / 8] = (uintptr_t)ldso_dl_open;          /* _dl_open */
    ro[0x278 / 8] = (uintptr_t)ldso_dl_close;         /* _dl_close */
    ro[0x280 / 8] = (uintptr_t)ldso_catch_error;      /* _dl_catch_error */
    ro[0x288 / 8] = (uintptr_t)ldso_error_free;       /* _dl_error_free */
    ro[0x290 / 8] = (uintptr_t)ldso_tls_get_addr_soft;/* _dl_tls_get_addr_soft */
    ro[0x298 / 8] = (uintptr_t)ldso_libc_freeres;     /* _dl_libc_freeres */
    ro[0x2a0 / 8] = (uintptr_t)ldso_find_object;      /* _dl_find_object */

    uint64_t *g = (uint64_t *)ldso_global;
    g[0xa80 / 8] = 1;                           /* _dl_nns */
    g[0xb18 / 8] = 1;                           /* dl_load_adds */
    /* dl_load_write_lock at +0xab8 stays all-zero (unlocked initial state) */

    /* The three pthread stack list heads must be self-referential (empty).
     * memset leaves them NULL, so pthread_create iterates _dl_stack_cache and
     * follows a NULL ->next -> SIGSEGV.  Offsets verified against glibc 2.41
     * (Debian): _dl_stack_used @0xb98, _dl_stack_user @0xba8,
     * _dl_stack_cache @0xbb8. */
    static const unsigned stack_lists[] = { 0xb98, 0xba8, 0xbb8 };
    for (size_t i = 0; i < sizeof stack_lists / sizeof stack_lists[0]; i++) {
        uintptr_t *head = (uintptr_t *)(ldso_global + stack_lists[i]);
        head[0] = (uintptr_t)(ldso_global + stack_lists[i]);  /* next = self */
        head[1] = (uintptr_t)(ldso_global + stack_lists[i]);  /* prev = self */
    }
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

static void *resolve_import_ldso(const char *name);
static void *ldso_lookup(const char *name);

static void ldso_signal_error(void) {
    static const char msg[] = "[DIAG-TLS] ldso_signal_error -> abort\n";
    ssize_t r = write(2, msg, sizeof(msg) - 1);
    (void)r;
    abort();
}

/* ───────────── dl* (dlopen/dlsym/dlerror/dlclose/dladdr) pro --ownall ─────
 * Guest glibc ma vlastni dlopen/dlsym implementovane nad _rtld_global, ktere
 * ale spoustime bez jejiho _dl_start -> _rtld_global je nulovy a kazde volani
 * guest dlopen/dlsym (napr. Rust uv hleda gnu_get_libc_version pres dlsym)
 * spadne na NULL dereference. Nahradime je nasi implementaci nad scopes:
 *   - symboly hledame v jiz nactenem scope (elf_scope_lookup) nebo v ldso
 *     override tabulce (resolve_import_ldso)
 *   - handle = elf_object_t* pro konkretni modul, nebo scope pro RTLD_DEFAULT
 */
static int  g_dl_err_valid;
static char g_dl_err[256];

/* TLS thread-pointery: bionic (host) a parrot (guest). Definovane vyse. */
extern uintptr_t g_tls_new_tp;

static elf_object_t *ldso_load_new(const char *file);
static void *override_lookup(const char *name);

static void dl_set_err(const char *msg) {
    snprintf(g_dl_err, sizeof g_dl_err, "%s", msg ? msg : "unknown dl error");
    g_dl_err_valid = 1;
}

static const char *dl_base_name(const char *p) {
    if (!p) return "";
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Najdi uz nacteny modul podle soname / basename / plne cesty. */
static elf_object_t *dl_find_loaded(const char *file) {
    if (!file || !g_crash_scope) return NULL;
    const char *want = dl_base_name(file);
    for (size_t i = 0; i < g_crash_scope->count; i++) {
        elf_object_t *m = g_crash_scope->mods[i];
        if (!m || !m->soname) continue;
        if (strcmp(m->soname, file) == 0) return m;
        if (strcmp(dl_base_name(m->soname), want) == 0) return m;
    }
    return NULL;
}

void *ldso_dlopen(const char *file, int mode) {
    (void)mode;
    uintptr_t saved = dl_tp_get();
    int sw = (g_tls_old_tp && saved != g_tls_old_tp);
    if (sw) dl_tp_set(g_tls_old_tp);
    void *ret = NULL;
    if (!file) {                 /* dlopen(NULL) = handle hlavniho programu */
        g_dl_err_valid = 0;
        ret = (void *)g_crash_scope;
    } else {
        elf_object_t *m = dl_find_loaded(file);
        if (m) { g_dl_err_valid = 0; ret = (void *)m; }
        else if (resolve_import_ldso(file)) { g_dl_err_valid = 0; ret = (void *)g_crash_scope; }
        else {
            /* Není načtený -> zkus ho own-loadnout z search paths (Python
             * import _ctypes potřebuje libffi.so.8 atd.). */
            m = ldso_load_new(file);
            if (m) { g_dl_err_valid = 0; ret = (void *)m; }
            else {
                char buf[256];
                snprintf(buf, sizeof buf, "%s: cannot open shared object file", file);
                dl_set_err(buf);
            }
        }
    }
    if (sw) dl_tp_set(saved);
    return ret;
}

/* RTLD_NEXT podpora: najdi modul ve scope, do ktereho patri adresa `a`. */
static elf_object_t *scope_mod_for_addr(elf_scope_t *s, uintptr_t a) {
    if (!s)
        return NULL;
    for (size_t i = 0; i < s->count; i++) {
        elf_object_t *m = s->mods[i];
        if (!m || !m->base_addr)
            continue;
        uintptr_t lo = (uintptr_t)m->base_addr;
        uintptr_t hi = lo + (m->total_size ? m->total_size : 0x400000);
        if (a >= lo && a < hi)
            return m;
    }
    return NULL;
}

/* Hledej `name` jen v jednom modulu (definovane globalni symboly). */
static void *mod_lookup_name(elf_object_t *m, const char *name) {
    if (!m || !m->dynsym || !m->dynstr)
        return NULL;
    for (size_t j = 0; j < m->dynsym_count; j++) {
        const Elf64_Sym *sym = &m->dynsym[j];
        if (sym->st_name == 0 || sym->st_shndx == SHN_UNDEF)
            continue;
        if (ELF64_ST_BIND(sym->st_info) == STB_LOCAL)
            continue;
        if (strcmp(m->dynstr + sym->st_name, name) != 0)
            continue;
        void *addr = (char *)m->base_addr + (sym->st_value - map_base_vaddr(m));
        if (ELF64_ST_TYPE(sym->st_info) == STT_GNU_IFUNC)
            addr = call_ifunc_resolver(addr);
        return addr;
    }
    return NULL;
}

void *ldso_dlsym(void *handle, const char *name) {
    if (!name) { dl_set_err("invalid symbol name"); return NULL; }
    uintptr_t saved = dl_tp_get();
    int sw = (g_tls_old_tp && saved != g_tls_old_tp);
    if (sw) dl_tp_set(g_tls_old_tp);
    void *ret = NULL;
    if (!handle) {                                  /* RTLD_DEFAULT */
        /* OVERRIDE prvni! Jinak by elf_scope_lookup nasel guest glibc
         * dlopen/dlsym (pracuji nad nulovym _rtld_global) a Python by si
         * pres dlsym(RTLD_DEFAULT,"dlopen") vytahl rozbitou verzi. */
        void *p = override_lookup(name);
        if (p) { g_dl_err_valid = 0; ret = p; }
        if (!ret && g_crash_scope) {
            p = elf_scope_lookup(g_crash_scope, name);
            if (p) { g_dl_err_valid = 0; ret = p; }
        }
        if (!ret) {
            p = resolve_import_ldso(name);
            if (p) { g_dl_err_valid = 0; ret = p; }
            else dl_set_err(name);
        }
    } else if (handle == (void *)-1) {              /* RTLD_NEXT */
        /* Sem chodi fakeroot (libfakeroot-tcp.so): next_<fn> = dlsym(RTLD_NEXT,
         * "<fn>"). Nesmime vratit symbol z modulu, ktery jej vola (jinak
         * nekonecna rekurze), ani nase override (ty patri "pred" nim).
         * Najdeme volajici modul a hledame az ZA nim. */
        uintptr_t caller = (uintptr_t)__builtin_return_address(0);
        elf_object_t *cm = g_crash_scope
                               ? scope_mod_for_addr(g_crash_scope, caller) : NULL;
        size_t start = 0;
        if (cm && g_crash_scope) {
            for (size_t i = 0; i < g_crash_scope->count; i++)
                if (g_crash_scope->mods[i] == cm) { start = i + 1; break; }
        }
        for (size_t i = start; g_crash_scope && i < g_crash_scope->count; i++) {
            void *rp = mod_lookup_name(g_crash_scope->mods[i], name);
            if (rp) { g_dl_err_valid = 0; ret = rp; break; }
        }
        if (!ret) {
            void *rp = resolve_import_ldso(name);
            if (rp) { g_dl_err_valid = 0; ret = rp; }
            else dl_set_err(name);
        }
    } else if (handle == (void *)g_crash_scope) {
        void *p = g_crash_scope ? elf_scope_lookup(g_crash_scope, name) : NULL;
        if (p) { g_dl_err_valid = 0; ret = p; }
        else dl_set_err(name);
    } else {
        /* handle je elf_object_t* vraceny nasim dlopen */
        void *addr = NULL;
        if (elf_resolve_symbol((elf_object_t *)handle, name, &addr) != SYM_NOT_FOUND
            && addr) {
            g_dl_err_valid = 0;
            ret = addr;
        } else {
            dl_set_err(name);
        }
    }
    if (sw) dl_tp_set(saved);
    return ret;
}

const char *ldso_dlerror(void) {
    if (!g_dl_err_valid) return NULL;
    g_dl_err_valid = 0;
    return g_dl_err;
}

int ldso_dlclose(void *h) { (void)h; return 0; }

/* Dl_info { const char *dli_fname; void *dli_fbase;
 *           const char *dli_sname; void *dli_saddr; } */
int ldso_dladdr(const void *addr, void *info_out) {
    const char **out = (const char **)info_out;
    if (!out) return 0;
    out[0] = NULL; out[1] = NULL; out[2] = NULL; out[3] = NULL;
    if (!g_crash_scope) return 0;
    uintptr_t a = (uintptr_t)addr;
    for (size_t i = 0; i < g_crash_scope->count; i++) {
        elf_object_t *m = g_crash_scope->mods[i];
        if (!m || !m->base_addr) continue;
        uintptr_t b = (uintptr_t)m->base_addr;
        if (a >= b && a < b + m->total_size) {
            out[0] = m->soname;
            out[1] = (const char *)b;
            return 1;
        }
    }
    return 0;
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
        tls_trace(name, (void *)ldso_signal_error, "ldso_lookup", "loader");
        return (void *)ldso_signal_error;
    }
    if (strcmp(name, "_dl_find_object") == 0)
        return (void *)ldso_find_object;
    if (strcmp(name, "_dl_rtld_di_serinfo") == 0)
        return (void *)ldso_signal_error;
    if (strcmp(name, "_dl_audit_preinit") == 0 ||
        strcmp(name, "_dl_audit_symbind_alt") == 0)
        return (void *)ldso_noop;
    if (strcmp(name, "dlopen") == 0 || strcmp(name, "dlopen64") == 0 ||
        strcmp(name, "__dlopen") == 0)
        return (void *)ldso_dlopen;
    if (strcmp(name, "dlsym") == 0 || strcmp(name, "__dlsym") == 0)
        return (void *)ldso_dlsym;
    if (strcmp(name, "dlerror") == 0)
        return (void *)ldso_dlerror;
    if (strcmp(name, "dlclose") == 0)
        return (void *)ldso_dlclose;
    if (strcmp(name, "dladdr") == 0)
        return (void *)ldso_dladdr;
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

static void *override_lookup(const char *name);  /* definováno níže; potřeba pro F2 lazy override */
static void *resolve_jmp_symbol(elf_object_t *obj, Elf64_Rela *r) {
    void *addr = NULL;
    const char *via = "?";
    size_t sym_idx = ELF64_R_SYM(r->r_info);
    if (sym_idx < obj->dynsym_count) {
        const Elf64_Sym *s = &obj->dynsym[sym_idx];
        const char *name = obj->dynstr + s->st_name;
        int is_ifunc = 0;
        if (s->st_shndx == SHN_UNDEF) {
            /* F2 override ma prioritu i v lazy resolvovani: explicitne
             * zaregistrovany shim (open/openat/stat/...) musi prebirat i
             * realny glibc symbol nalezny pres elf_scope_find. Bez toho by
             * lazy JUMP_SLOT sel rovnou na glibc a shim se nikdy nezavolal. */
            void *ov = override_lookup(name);
            addr = ov;
            via = "override";
            if (!addr) {
                via = "scope";
                if (obj && obj->scope) {
                    const Elf64_Sym *fs = NULL;
                    elf_object_t *m = elf_scope_find(obj->scope, name, &fs);
                    if (m && fs) {
                        if (ELF64_ST_TYPE(fs->st_info) == STT_GNU_IFUNC)
                            is_ifunc = 1;
                        addr = (char *)m->base_addr + (fs->st_value - map_base_vaddr(m));
                    }
                }
                if (!addr) {
                    via = "import";
                    addr = elf_resolve_import(obj, name);
                }
            }
        } else {
            via = "defined";
            addr = va(obj, s->st_value);
        }
        if (is_ifunc && getenv("ELF_LOADER_RELOC_TRACE"))
            fprintf(stderr, "[ifunc] %s in %s -> resolver %p\n",
                    name, obj->soname ? obj->soname : "?", addr);
        if (addr && is_ifunc)
            addr = call_ifunc_resolver(addr);
        tls_trace(name, addr, via, obj->soname);
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
    if (elf_debug())
        fprintf(stderr, "[dbg] override+ %s -> %p (count=%zu)\n",
                name, fn, override_count);
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

/* Prebije `svc #0` v guest modulu pro dany syscall nr. Slouzi pro syscally,
 * ktere app seccomp KILLuje (KILL nejde prebit filtrem ani SIGSYS handlerem):
 * rseq (293), set_robust_list (99, delka 24) a clone3 (435).
 *
 * err == 0  -> `svc #0` se prebije na NOP  (syscall se vubec neprovede)
 * err  > 0  -> `svc #0` se prebije na `movn x0,#(err-1)` -> x0 = -err,
 *              takze se volajicimu vraci -errno (napr. -ENOSYS). To je
 *              potreba u clone3: Rust/glibc si pri ENOSYS fallbackne na clone().
 *
 * Hleda sekvenci `movz x8,#nr` (pripadne `mov x8,#nr` = movz) a do +8
 * instrukci za ni najde `svc #0`. */
void elf_patch_syscall_sites(elf_object_t *m, long nr, long err) {
    if (!m || nr < 0 || nr > 0xffff)
        return;
    const uint32_t movz_x8 = 0xd2800000u | ((uint32_t)nr << 5) | 8u;
    uint32_t repl;
    if (err > 0)
        repl = 0x92800000u | (((uint32_t)(err - 1) & 0xffffu) << 5); /* movn x0,#err-1 */
    else
        repl = 0xd503201fu;                                          /* nop */
    int patched = 0;
    for (int i = 0; i < m->phdr_count; i++) {
        if (m->phdr[i].p_type != PT_LOAD)
            continue;
        if (!(m->phdr[i].p_flags & PF_X))
            continue;
        size_t seg_vaddr = m->phdr[i].p_vaddr;
        size_t seg_sz = m->phdr[i].p_memsz;
        uint32_t *seg = (uint32_t *)va(m, seg_vaddr);
        size_t n_ins = seg_sz / 4;
        for (size_t k = 0; k + 8 < n_ins; k++) {
            if (seg[k] != movz_x8)
                continue;
            /* hledej svc #0 do +8 instrukci za movz (nesmi pres jinou movz) */
            for (size_t j = k + 1; j < k + 9 && j < n_ins; j++) {
                if (seg[j] == 0xd4000001u) {  /* svc #0 */
                    uintptr_t ins_addr = (uintptr_t)&seg[j];
                    uintptr_t pg = ins_addr & ~(uintptr_t)(PAGE_SIZE - 1);
                    if (mprotect((void *)pg, PAGE_SIZE,
                                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                        seg[j] = repl;
                        __builtin___clear_cache((char *)&seg[j],
                                                (char *)&seg[j] + 4);
                        mprotect((void *)pg, PAGE_SIZE,
                                 PROT_READ | PROT_EXEC);
                        patched++;
                    }
                    break;
                }
                /* Mezi movz x8,#nr a svc povolime instrukce, ktere NEZAPISUJI
                 * do x8 (Rd != x8) - typicky nastaveni argumentu x0-x7.
                 * Pokud nekdo x8 prepsal (Rd == x8), svc patri jinemu syscallu
                 * a jeho prepsanim bychom rozbili volani (napr. _cffi_backend
                 * mel konstantu blizko svc). nop/bti preskocime vzdy. */
                if (seg[j] == 0xd503201fu /* nop */ ||
                    (seg[j] & 0xffffff1fu) == 0xd503241fu /* bti c/j */)
                    continue;
                if ((seg[j] & 0x1fu) == 8u)   /* Rd == x8/w8 -> x8 prepsan */
                    break;
            }
        }
    }
    if (elf_debug())
        fprintf(stderr, "[patch] syscall %ld (err=%ld): prebito %d svc#0 v %s\n",
                nr, err, patched, m->soname ? m->soname : "?");
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
        const char *dr = find_distro_root(origin_dir);
        rootfs_base = strdup(dr ? dr : "/");
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

/* Resolve symlinks on a host path that lives under ROOTFS. When the F2
 * seccomp filter is absent the kernel resolves absolute symlink targets
 * against the real root (ENOENT). This helper walks the chain manually,
 * prepending ROOTFS to absolute targets, so elf_load can open the final
 * real file without the filter. Uses only lstat/readlink (no filter needed). */
static void resolve_symlinks_under_root(const char *path, char *out, size_t outsz) {
    const char *root = getenv("ROOTFS");
    if (!root || !root[0] || !path || !path[0]) {
        if (out && outsz) { strncpy(out, path ? path : "", outsz - 1); out[outsz-1] = 0; }
        return;
    }
    size_t rl = strlen(root);
    char cur[4096];
    strncpy(cur, path, sizeof(cur)-1); cur[sizeof(cur)-1] = 0;
    for (int depth = 0; depth < 32; depth++) {
        struct stat lst;
        if (lstat(cur, &lst) != 0 || !S_ISLNK(lst.st_mode)) {
            strncpy(out, cur, outsz-1); out[outsz-1] = 0;
            return;
        }
        char linkbuf[4096];
        ssize_t lr = readlink(cur, linkbuf, sizeof(linkbuf)-1);
        if (lr < 0) { strncpy(out, cur, outsz-1); out[outsz-1] = 0; return; }
        linkbuf[lr] = 0;
        char next[4096];
        if (linkbuf[0] == '/') {
            /* absolute target: prepend ROOTFS */
            if (rl + strlen(linkbuf) + 1 < sizeof(next)) {
                memcpy(next, root, rl);
                strcpy(next + rl, linkbuf);
            } else { strncpy(out, cur, outsz-1); out[outsz-1] = 0; return; }
        } else {
            /* relative target: resolve against symlink's directory */
            strncpy(next, cur, sizeof(next)-1); next[sizeof(next)-1] = 0;
            char *sl = next + strlen(next);
            while (sl > next + 1 && sl[-1] != '/') sl--;
            *sl = 0;
            if (strlen(next) + strlen(linkbuf) + 1 < sizeof(next))
                strcat(next, linkbuf);
            else { strncpy(out, cur, outsz-1); out[outsz-1] = 0; return; }
        }
        strncpy(cur, next, sizeof(cur)-1); cur[sizeof(cur)-1] = 0;
    }
    strncpy(out, cur, outsz-1); out[outsz-1] = 0;
}

elf_object_t *elf_load(const char *path) {
    if (is_android_stub(path)) {
        fprintf(stderr,
                "[-] %s: Android stub (symlink to /bin/true) — not a runnable binary.\n"
                "    Use the real distro binary, or run via: gbsh --chroot <rootfs> <bin>\n",
                path);
        return NULL;
    }
    /* Resolve symlinks explicitly (e.g. awk -> /etc/alternatives/awk -> gawk)
     * so that absolute symlink targets are resolved against ROOTFS rather
     * than the real root. This keeps elf_load working without the F2
     * seccomp filter (needed for re-exec children that inherit the filter
     * but lose the SIGSYS handler across execve). */
    char resolved[4096];
    resolve_symlinks_under_root(path, resolved, sizeof resolved);
    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[-] open(%s): %s\n", resolved, strerror(errno));
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
        size_t al = obj->phdr[i].p_align ? obj->phdr[i].p_align : 1;
        obj->tls_memsz = obj->phdr[i].p_memsz;
        obj->has_tls = 1;
        /* Zivy TLS template v obraze modulu: po elf_relocate() je jiz
         * prelozeny (R_AARCH64_RELATIVE uvnitr .tdata), takze se z nej da
         * kopirovat pri kazdem novem vlakne. */
        obj->tdata_src = (char *)base +
                         (obj->phdr[i].p_vaddr - map_base_vaddr_);
        obj->tdata_filesz = obj->phdr[i].p_filesz;
        elf_tls_assign(obj, al);
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
    /* Fallback: hlavni exe (Python extension moduly potrebuji PyExc_*,
     * PyTuple_Type, _PyRuntime z python3.13, ktery neni v mods). */
    elf_object_t *e = s->exe;
    if (e && e->dynsym && e->dynstr) {
        for (size_t j = 0; j < e->dynsym_count; j++) {
            const Elf64_Sym *sym = &e->dynsym[j];
            if (sym->st_name == 0 || sym->st_shndx == SHN_UNDEF)
                continue;
            if (ELF64_ST_BIND(sym->st_info) == STB_LOCAL)
                continue;
            if (strcmp(e->dynstr + sym->st_name, name) != 0)
                continue;
            if (out_sym)
                *out_sym = sym;
            return e;
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

/* Najde distro root: prochazi origin_dir smerem nahoru a vraci nejvyssi
 * adresar d, pro ktery existuje d/usr/lib/aarch64-linux-gnu (multiarch libdir =
 * podpis distro rootfsu). Funguje pro klasicky exe (/distro/usr/bin/exe) i pro
 * binarky mimo /usr/bin ci /bin (napr. /distro/root/.nvm/.../bin/node): v obou
 * pripadech je rootfs base ten adresar, ktery obsahuje usr/lib. Vraci static
 * buf (rootfs base) nebo NULL. */
static const char *find_distro_root(const char *origin_dir) {
    static char fbuf[2048];
    if (!origin_dir || !*origin_dir) return NULL;
    char path[2048];
    size_t n = strlen(origin_dir);
    if (n == 0 || n >= sizeof path) return NULL;
    memcpy(path, origin_dir, n + 1);
    for (;;) {
        char test[2048];
        int tl = snprintf(test, sizeof test, "%s/usr/lib/aarch64-linux-gnu", path);
        if (tl < 0 || (size_t)tl >= sizeof test) return NULL;
        if (access(test, F_OK) == 0) {
            size_t pl = strlen(path);
            if (pl >= sizeof fbuf) return NULL;
            memcpy(fbuf, path, pl + 1);
            return fbuf;
        }
        char *slash = strrchr(path, '/');
        if (!slash) return NULL;
        if (slash == path) { fbuf[0] = '/'; fbuf[1] = 0; return fbuf; }
        *slash = 0;
    }
}

/* Odvození distro lib cest z origin_dir binárky v distro layoutu:
 *   …/distro/usr/bin/exe → …/distro/{usr/lib/aarch64-linux-gnu,
 *   lib/aarch64-linux-gnu, usr/lib, lib}
 * Umožňuje najít libc.so.6 apod. bez LD_LIBRARY_PATH i bez ELF_ROOTFS. */
static char *derive_distro_libdirs(const char *origin_dir) {
    static char buf[2048];
    const char *base = find_distro_root(origin_dir);
    if (!base) return NULL;
    size_t bl = strlen(base);

    if (bl + 256 >= sizeof buf) return NULL;
    memcpy(buf, base, bl); buf[bl] = 0;

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
        /* DT_RUNPATH / DT_RPATH modulu (s $ORIGIN). Python balicky (numpy)
         * maji RPATH=$ORIGIN/../../numpy.libs a sve .so (libscipy_openblas)
         * drzi mimo standardni libdirs - bez tohoto by je loader nenasel a
         * symboly zustaly NULL -> SIGSEGV. Poradi: RUNPATH prvni (nejuzsi). */
        char *rpath_exp = NULL;
        {
            char *runpath = NULL, *rpath = NULL;
            for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
                if (d->d_tag == DT_RUNPATH)
                    runpath = dynstr + d->d_un.d_val;
                else if (d->d_tag == DT_RPATH)
                    rpath = dynstr + d->d_un.d_val;
            }
            const char *rp = runpath ? runpath : rpath;
            if (rp && rp[0])
                rpath_exp = expand_dirs(rp, m->origin_dir ? m->origin_dir : ".");
        }
        char *dl = derive_distro_libdirs(m->origin_dir ? m->origin_dir : "");
        size_t need = strlen(search) + strlen(sys_libdirs()) + 16
                    + (dl ? strlen(dl) : 0)
                    + (rpath_exp ? strlen(rpath_exp) : 0);
        char *osearch = malloc(need);
        if (rpath_exp && rpath_exp[0])
            sprintf(osearch, "%s:%s:%s:%s", rpath_exp, dl ? dl : "",
                    search, sys_libdirs());
        else if (dl)
            sprintf(osearch, "%s:%s:%s", dl, search, sys_libdirs());
        else
            sprintf(osearch, "%s:%s", search, sys_libdirs());
        free(rpath_exp);
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
/* init_fn_t, g_pending_inits a g_pending_count jsou deklarovane vyse
 * (u ldso_dl_open_impl) - runtime _dl_open je take musi spoustet. */
static size_t g_pending_cap;

/* libc TLS per-thread state: locale pointer + ctype tables leží v
 * [TP + slot_off] slotech (offsety v libc .data na 0x1aff40 / 0x1afd58).
 * Náš region je zeroed -> strtol/isalpha atd. dereferencují NULL.
 * Resolvujeme uselocale(NULL) (= global locale) a __ctype_init()
 * přímo z own-loadeného libc a voláme pod parrot TP. */
static void *(*g_libc_uselocale)(void *);
static void (*g_libc_ctype_init)(void);
extern uintptr_t g_tls_new_tp;
extern uintptr_t g_tls_old_tp;

/* Forward declaration: raw_syscall6 defined later but used in
 * diag.txt writes in early init/handler. */
static long raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);

/* Volá se z asm (elf_final_jump) pod parrot TP. Žádný bionic kód/malloc. */
static void install_sigsys_handler_now(void);
static int g_f2_filter_active = 0;

/* Skip flagy cachovane pod bionickym TP - elf_run_pending_inits bezi pod
 * parrot TP a bionicke getenv() by tam cetlo parrot TLS (undefined result). */
static int g_skip_inits = 0;
static int g_skip_locale = 0;
static int g_keep_handlers = 0;

void elf_run_pending_inits(void) {
    { int _fd = raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x441L, 0644L, 0, (long)F2_SENTINEL); if (_fd >= 0) { const char _m[] = "INITS-START\n"; raw_syscall6(64, _fd, (long)(unsigned long)_m, sizeof(_m) - 1, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); } }
    if (g_tls_new_tp) {
        if (!g_skip_locale) {
            if (g_libc_uselocale)
                g_libc_uselocale(NULL);      /* thread locale = _nl_global_locale */
            if (g_libc_ctype_init)
                g_libc_ctype_init();         /* ctype_b/tolower sloty pro tento TP */
        }
    }
    {
        char _b[48]; int _i = 0; const char *_p = "INITS-RUN n=";
        while (*_p) _b[_i++] = *_p++;
        unsigned long _n = (unsigned long)g_pending_count; char _t[24]; int _ti = 0;
        if (_n == 0) _t[_ti++] = '0';
        while (_n > 0) { _t[_ti++] = (char)('0' + (_n % 10)); _n /= 10; }
        while (_ti > 0) _b[_i++] = _t[--_ti];
        _b[_i++] = '\n';
        int _fd = (int)raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x441L, 0644L, 0, (long)F2_SENTINEL);
        if (_fd >= 0) { raw_syscall6(64, _fd, (long)(unsigned long)_b, _i, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); }
    }
    if (!g_skip_inits)
    for (size_t i = 0; i < g_pending_count; i++) {
        init_fn_t fn = g_pending_inits[i];
        fn(elf_init_argc, elf_init_argv, elf_init_envp);
    }
    /* po initech: libc/program může mít přepsán SIGSEGV handler (procps
     * ps/top) → reinstalovat náš fault dump handler pro diagnostiku */
    if (g_keep_handlers)
        elf_install_fault_handlers();
    /* Guest glibc při inicializaci přepíše SIGSYS handler na default ->
     * F2 path-translation (seccomp TRAP na openat) by zabila proces.
     * Reinstalujeme náš handler. sigaction je bionický (čte bionic TLS),
     * takže na dobu volání přepneme TP na bionic. */
    if (g_f2_filter_active && g_tls_old_tp) {
        uintptr_t _s; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(_s));
        if (_s != g_tls_old_tp) {
            __asm__ volatile("msr tpidr_el0, %0" : : "r"(g_tls_old_tp));
            install_sigsys_handler_now();
            __asm__ volatile("msr tpidr_el0, %0" : : "r"(_s));
        } else {
            install_sigsys_handler_now();
        }
    }
    { int _fd = raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x441L, 0644L, 0, (long)F2_SENTINEL); if (_fd >= 0) { const char _m[] = "INITS-DONE\n"; raw_syscall6(64, _fd, (long)(unsigned long)_m, sizeof(_m) - 1, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); } }
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
        /* glibc 2.41: flag na libc+0x1b02a0 ridi, zda pthread_create pouzije
         * clone3 (hodnota 1 = ano). Android app seccomp vraci pro clone3
         * SECCOMP_RET_KILL — KILL obchazi SIGSYS handler a nelze ho prebit
         * ani stackovanym filtrem (vyhrava nejnizsi akce). Vynutime fallback
         * na clone(): pri hodnote != 1 jde glibc __clone3 rovnou na fallback
         * vetev (disasm: ldr w2,[x19]; cmp w2,#1; b.ne fallback). Offset je
         * specificky pro glibc 2.41; guard: hodnota musi byt presne 1. */
        if (m->total_size > 0x1b02a4) {
            uint32_t *clone3_ok = (uint32_t *)va(m, 0x1b02a0);
            if (*clone3_ok == 1)
                *clone3_ok = 0;
        }
        /* __libc_early_init normally computes __default_pthread_attr's stack
         * size from RLIMIT_STACK; the loader skips it (it touches GLRO state
         * we don't emulate fully), so the default stays 0 and the first
         * pthread_create fails "allocate_stack: size != 0".  Set it directly:
         * the field lives right after __pthread_keys (0x4050 bytes past it),
         * verified against glibc 2.41 (Debian) disassembly. */
        void *keys = NULL;
        sym_status_t ks = lookup_table(m->dynsym, m->dynstr, m->dynsym_count,
                                       "__pthread_keys", &keys, m);
        if (elf_debug())
            fprintf(stderr, "[dbg] __pthread_keys: st=%d addr=%p\n", ks, keys);
        if (ks == SYM_DEFINED && keys) {
            *(size_t *)((char *)keys + 0x4050) = 8 * 1024 * 1024;
            if (elf_debug())
                fprintf(stderr, "[dbg] default_stacksize set @%p = %zu\n",
                        (char *)keys + 0x4050, *(size_t *)((char *)keys + 0x4050));
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
    /* DIAG: pro kazdy modul soname + DT_INIT + DT_INIT_ARRAYSZ */
    {
        char _b[256]; int _i = 0; const char *_p = "MOD ";
        while (*_p) _b[_i++] = *_p++;
        const char *sn = m->soname ? m->soname : "?";
        for (const char *q = sn; *q && _i < 100; q++) _b[_i++] = *q;
        const char *k1 = " init=0x"; while (*k1) _b[_i++] = *k1++;
        static const char hx[] = "0123456789abcdef";
        char _h[17]; int _hi; unsigned long _v;
        _v = (unsigned long)init; _hi = 0;
        if (!_v) _h[_hi++] = '0';
        while (_v) { _h[_hi++] = hx[_v & 15]; _v >>= 4; }
        while (_hi) _b[_i++] = _h[--_hi];
        const char *k2 = " iasz=0x"; while (*k2) _b[_i++] = *k2++;
        _v = (unsigned long)init_arraysz; _hi = 0;
        if (!_v) _h[_hi++] = '0';
        while (_v) { _h[_hi++] = hx[_v & 15]; _v >>= 4; }
        while (_hi) _b[_i++] = _h[--_hi];
        _b[_i++] = '\n';
        raw_syscall6(64, 2, (long)(unsigned long)_b, _i, 0, 0, (long)F2_SENTINEL);
    }
    if (init_array && init_arraysz) {
        uint64_t *arr = (uint64_t *)va(m, init_array);
        size_t n = init_arraysz / sizeof(uint64_t);
        for (size_t i = 0; i < n; i++)
            elf_queue_init((init_fn_t)arr[i]);
    }
}

/* Verejny wrapper: zaradi DT_INIT + init_array hlavniho exe do fronty.
 * elf_load() sam inity nequeueuje (exe se relokuje az v main.c), takze
 * run_ownall musi tuhle funkci zavolat PO elf_relocate(obj). Bez toho
 * konstruktory hlavniho programu nebezi vubec (node ma OpenSSL staticky ->
 * bez OSSL ctoru zadny provider -> CSPRNG assert). */
void elf_queue_module_inits(elf_object_t *m) {
    size_t _before = g_pending_count;
    run_module_init(m);
    {
        char _b[200]; int _i = 0;
        const char *_p = "QINIT "; while (*_p) _b[_i++] = *_p++;
        const char *sn = (m && m->soname) ? m->soname : "?";
        for (const char *q = sn; *q && _i < 120; q++) _b[_i++] = *q;
        const char *_p2 = " add="; while (*_p2) _b[_i++] = *_p2++;
        char _t[24]; int _ti = 0; unsigned long _n = (unsigned long)(g_pending_count - _before);
        if (!_n) _t[_ti++] = '0';
        while (_n) { _t[_ti++] = (char)('0' + (_n % 10)); _n /= 10; }
        while (_ti) _b[_i++] = _t[--_ti];
        _b[_i++] = 10;
        raw_syscall6(64, 2, (long)(unsigned long)_b, _i, 0, 0, (long)F2_SENTINEL);
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
    char resolved[4096];
    resolve_symlinks_under_root(path, resolved, sizeof resolved);
    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[-] open(%s): %s\n", resolved, strerror(errno));
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
        size_t al = m->phdr[i].p_align ? m->phdr[i].p_align : 1;
        /* Zivy TLS template v obraze modulu (relokovany v elf_relocate(m)). */
        m->tdata_src = (char *)base + (m->phdr[i].p_vaddr - mbv);
        m->tdata_filesz = m->phdr[i].p_filesz;
        m->tls_memsz = m->phdr[i].p_memsz;
        m->has_tls = 1;
        elf_tls_assign(m, al);
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

    /* Dynamicky nacteny modul s PT_TLS (i jeho zavislosti) potrebuje mit
     * .tdata zkopirovanou do AKTUALNIHO threadu. Pri startu to dela
     * elf_setup_own_tls pro vsechny moduly; pri runtime loadu (Python
     * extension .so jako numpy/_multiarray_umath + libscipy_openblas) uz
     * hlavni TLS region existuje, takze kopirujeme rovnou. Bez toho zustane
     * TLS zavislosti smeti -> pthread_key_create/delete padá. */
    if (g_tls_new_tp && m->has_tls && m->tls_offset)
        elf_tls_add_module_to_thread(m);

    /* Pozn.: .tdata se uz NEKopiruje na read_tp()+offset (tls_offset je nyni
     * maly offset od TP, platny az pro parrot TP). O inicializaci se stara
     * elf_setup_own_tls (main thread) a ldso_tls.c (nove thready). */
    if (elf_debug()) {
        for (int i = 0; i < m->phdr_count; i++) {
            if (m->phdr[i].p_type != PT_TLS || !m->has_tls)
                continue;
            printf("[dbg2] tls-reg off=%llx filesz=%llx memsz=%llx\n",
                   (unsigned long long)m->tls_offset,
                   (unsigned long long)m->phdr[i].p_filesz,
                   (unsigned long long)m->phdr[i].p_memsz);
            break;
        }
        printf("[dbg2] tls-done\n");
    }

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
    /* App seccomp KILLuje rseq(293), set_robust_list(99) a clone3(435).
     * Staticke moduly se patchi pri startu (main.c), ale dynamicky loadovane
     * (zsh zmodload, NSS, Python extension) se musi patchovat TADY - jinak
     * jejich konstruktor/inicializace syscall KILLne -> "Bad system call". */
    if (!getenv("ELF_LOADER_NO_PATCH") && m->phdr && m->phdr_count > 0) {
        elf_patch_syscall_sites(m, 99, 0);    /* set_robust_list -> NOP */
        elf_patch_syscall_sites(m, 293, 0);   /* rseq            -> NOP */
        elf_patch_syscall_sites(m, 435, 38);  /* clone3          -> -ENOSYS */
        /* setfsuid(151)/setfsgid(152): libtinfo (_nc_safe_fopen), glibc
         * login_tty apod. je volaji PRIMO (ne pres nas override v PLT).
         * App profil na ne vraci TRAP/KILL -> "Bad system call". Prebitim
         * na NOP je zcela vyradime (navratova hodnota se ignoruje). */
        elf_patch_syscall_sites(m, 151, 0);
        elf_patch_syscall_sites(m, 152, 0);
    }
    if (elf_debug())
        printf("[+] own-loaded module: %s (base %p, %zu dynsym)\n", path,
           (void *)base, m->dynsym_count);
    fflush(stdout);
    return m;
}
/* Runtime dlopen: guest požádal o modul, který ještě není načtený
 * (typicky Python import _ctypes -> libffi.so.8). Sestavíme search path
 * z distro libdirs a own-loadneme ho do crash scope.
 * VOLÁNO s bionickým TP (ldso_dlopen už přepnul) — elf_load_shared je
 * loaderuv bionický kód. Inity guest modulu ale musí běžet pod parrot TP,
 * takže je spustíme s dočasným přepnutím na g_tls_new_tp a zpět. */
static elf_object_t *ldso_load_new(const char *file) {
    if (!g_crash_scope || !file || !file[0])
        return NULL;

    char *search = NULL;
    const char *root = getenv("ROOTFS");
    size_t rl = (root && root[0]) ? strlen(root) : 0;
    char pbuf[4096];
    /* Případy:
     *  a) file je už device-absolutní cesta (Python sys.path) -> zkusit přímo
     *  b) file je guest-absolutní (/usr/lib/...) -> ROOTFS + file
     *  c) relativní / se '/' -> ROOTFS + file
     *  d) holé soname -> hledat v libdirs */
    if (access(file, R_OK) == 0) {
        search = strdup(file);
    } else if (file[0] == '/') {
        snprintf(pbuf, sizeof pbuf, "%s%s", root ? root : "", file);
        if (access(pbuf, R_OK) == 0)
            search = strdup(pbuf);
        else if (rl && strncmp(file, root, rl) == 0)
            search = strdup(file);
    } else if (strchr(file, '/')) {
        snprintf(pbuf, sizeof pbuf, "%s%s", root ? root : "", file);
        if (access(pbuf, R_OK) == 0)
            search = strdup(pbuf);
    }
    if (!search) {
        char *dl = derive_distro_libdirs("");
        char osearch[4096];
        if (dl)
            snprintf(osearch, sizeof osearch, "%s:%s", dl, sys_libdirs());
        else
            snprintf(osearch, sizeof osearch, "%s", sys_libdirs());
        search = find_in_paths(file, osearch);
    }
    if (!search)
        return NULL;

    size_t prev_count = g_pending_count;
    elf_object_t *m = elf_load_shared(search, g_crash_scope);
    free(search);
    if (!m)
        return NULL;
    /* Novy modul muze mit PT_TLS -> zkopiruj jeho .tdata do aktualniho
     * threadu (region ma fixni rezervu, takze se vejde). */
    if (m->has_tls)
        elf_tls_add_module_to_thread(m);

    /* Spusť inity nově přidané do fronty pod parrot TP (guest kód). */
    if (!getenv("ELF_LOADER_NO_INITS") && g_pending_count > prev_count) {
        uintptr_t save2 = dl_tp_get();
        if (g_tls_new_tp)
            dl_tp_set(g_tls_new_tp);
        for (size_t i = prev_count; i < g_pending_count; i++) {
            init_fn_t fn = g_pending_inits[i];
            if (fn)
                fn(elf_init_argc, elf_init_argv, elf_init_envp);
        }
        dl_tp_set(save2);
    }
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
    void *sym = override_lookup(name);
    /* Override ma prioritu pred host/resolvovanim: explicitne zaregistrovany
     * shim (F2 path-translation) musi prebirat i host ld.so symboly jako
     * "open". Bez toho by resolve_import_ldso vracel bionicky open a shim
     * se nikdy nezavolal. */
    if (sym) {
        tls_trace(name, sym, "override", obj ? obj->soname : NULL);
        return sym;
    }
    sym = resolve_import_ldso(name);
    if (sym) {
        tls_trace(name, sym, "ldso", obj ? obj->soname : NULL);
        return sym;
    }
    if (obj && obj->scope) {
        sym = elf_scope_lookup(obj->scope, name);
        if (sym) {
            tls_trace(name, sym, "import/scope", obj->soname);
            return sym;
        }
    }
    for (size_t i = 0; obj && i < obj->handle_count; i++) {
        if (!obj->handles[i])
            continue;
        void *h = dlsym(obj->handles[i], name);
        if (h) {
            tls_trace(name, h, "handle", obj->soname);
            return h;
        }
    }
    /* Host fallback jen pro non-ownall flow (--run/--own/--shim): proces
     * běží pod host libc, takže její symboly (printf, __libc_start_main...)
     * můžeme použít přímo. --ownall (parrot svět) musí zůstat strict —
     * tam by bionic symboly tiše rozbily glibc program. */
    if (!elf_own_deps) {
        void *h2 = dlsym(RTLD_DEFAULT, name);
        if (h2) {
            tls_trace(name, h2, "host", obj ? obj->soname : NULL);
            return h2;
        }
    }
    tls_trace(name, NULL, "none", obj ? obj->soname : NULL);
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

/* R_AARCH64_COPY: symbol je v exe definovan jen jako rezervace v .bss
 * (stdin/stdout/stderr/__environ/__stack_chk_guard). Skutecna definice je
 * v libc (nebo v ldso override). Zkopiruj obsah do ciloveho slotu, aby exe
 * videl platny FILE* / hodnoty. Bez toho zustane stdin=NULL -> fileno(NULL) crash. */
static int do_copy_reloc(elf_object_t *obj, Elf64_Rela *r, void *where) {
    size_t sym_idx = ELF64_R_SYM(r->r_info);
    if (sym_idx >= obj->dynsym_count)
        return 0;
    const Elf64_Sym *s = &obj->dynsym[sym_idx];
    const char *name = obj->dynstr + s->st_name;
    size_t sz = s->st_size ? (size_t)s->st_size : sizeof(uint64_t);

    void *src = NULL;
    if (obj->scope) {
        for (size_t i = 0; i < obj->scope->count && !src; i++) {
            elf_object_t *m = obj->scope->mods[i];
            if (!m || m == obj)
                continue;
            if (!m->dynsym || !m->dynstr)
                continue;
            for (size_t j = 0; j < m->dynsym_count; j++) {
                const Elf64_Sym *ds = &m->dynsym[j];
                if (ds->st_name == 0 || ds->st_shndx == SHN_UNDEF)
                    continue;
                if (ELF64_ST_BIND(ds->st_info) == STB_LOCAL)
                    continue;
                if (strcmp(m->dynstr + ds->st_name, name) != 0)
                    continue;
                src = (char *)m->base_addr + (ds->st_value - map_base_vaddr(m));
                break;
            }
        }
    }
    if (!src)
        src = override_lookup(name);   /* __stack_chk_guard, __rseq_* apod. */
    if (src) {
        memcpy(where, src, sz);
        if (elf_debug())
            fprintf(stderr, "[COPY] %s <- %p (%zu B)\n", name, src, sz);
        return 1;
    }
    fprintf(stderr, "[WARN] COPY reloc unresolved: %s in %s\n",
            name, obj->soname ? obj->soname : "EXE");
    return 0;
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
        case R_AARCH64_COPY:
            count += do_copy_reloc(obj, r, where);
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
    (void)exe;
    (void)scope;
    elf_tls_ctx_t ctx = {0};
    uintptr_t host_tp = read_tp();
    ctx.old_tp = host_tp;

    /* Cachuj skip-flagy TED - jsme pod bionickym TP, getenv() funguje.
     * elf_run_pending_inits() bezi pod parrot TP, kde by bionicke getenv()
     * cetlo parrot TLS a vracelo nesmysl (inity by se preskocily). */
    {
        const char *v;
        v = getenv("ELF_LOADER_NO_INITS");
        g_skip_inits = (v && v[0] && v[0] != '0') ? 1 : 0;
        v = getenv("ELF_LOADER_NO_LOCALE");
        g_skip_locale = (v && v[0] && v[0] != '0') ? 1 : 0;
        v = getenv("ELF_LOADER_KEEP_HANDLERS");
        g_keep_handlers = (v && v[0] && v[0] != '0') ? 1 : 0;
        /* Diagnostika do diag.txt (raw, TP-independent) */
        char _b[64]; int _i = 0; const char *_p = "ENV-CACHE skip_inits=";
        while (*_p) _b[_i++] = *_p++;
        _b[_i++] = (char)('0' + g_skip_inits);
        const char *_p2 = " skip_locale=";
        while (*_p2) _b[_i++] = *_p2++;
        _b[_i++] = (char)('0' + g_skip_locale);
        _b[_i++] = '\n';
        int _fd = (int)raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x441L, 0644L, 0, (long)F2_SENTINEL);
        if (_fd >= 0) { raw_syscall6(64, _fd, (long)(unsigned long)_b, _i, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); }
    }

    /* Vsechny TLS moduly (exe + scope) jsou registrovane pres elf_tls_assign
     * s malymi kladnymi offsety od TP (aarch64 TLS_DTV_AT_TP). */
    size_t span = 0;
    for (size_t i = 0; i < g_tls_mod_count; i++) {
        elf_object_t *m = g_tls_mods[i];
        if (!m)
            continue;
        size_t end = (size_t)m->tls_offset + m->tls_memsz;
        if (end > span)
            span = end;
    }
    /* FIX (btop/apt crash): i kdyz exe ani moduly nemaji PT_TLS, MUSIME vzdy
     * alokovat vlastni region s pthread struct headroomem a prepnout TP.
     * Modulove init_array (libstdc++ atd.) bezi jeste PRED trampolinou - pod
     * bionickym TPIDR_EL0 narazi parrot libc pri prvnim dotku pod TP-0x720
     * (_pthread_cleanup_push: cleanup listy, cancellable futex path) na guard
     * page [anon:stack_and_tls] -> SIGSEGV. Nulovana pthread struct v regionu
     * je validni prazdny stav (cleanup list head = NULL). */
    /* DTV dimenzujeme na MAX modulu (dynamicky load muze pridat dalsi). */
    size_t dtv_bytes = (2 + ELF_MAX_TLS_MODS + 16) * 16;
    size_t need_end = (size_t)ELF_TLS_RESERVE + ELF_RSEQ_SIZE + dtv_bytes;
    if (need_end < span) need_end = span;
    size_t size = ALIGN_UP(TLS_PRE_TCB_SIZE + need_end + 0x1000, PAGE_SIZE);
    void *region = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        return ctx;
    memset(region, 0, size);

    uintptr_t new_tp = (uintptr_t)region + TLS_PRE_TCB_SIZE;
    /* region was zeroed above: struct pthread occupies [region, new_tp).
     * Pozn.: malloc thread_arena slot (TP-offset z libc .data @0x1afd68) zůstává
     * NULL = "uninitialized" -> glibc malloc si sám vezme main_arena. */

    /* glibc drzi v struct pthread (TP-0x720) `tid` na offsetu 0xD0.
     * Realny ld.so/exec ho naplni pres set_tid_address; nas loader ho
     * nechal nulovy, a pak pthread_rwlock_rdlock/wrlock porovnava
     * __writer s THREAD_SELF->tid -> pri odemcenem zamku (__writer==0)
     * vyjde 0==0 a glibc vraci falesny EDEADLK (35). To rozbiji OpenSSL
     * zamky (store se neinicializuje -> zadny provider -> node CSPRNG
     * assert) i pthread_mutex (__owner==0). Napevno sem zapiseme kernel
     * tid (aarch64 gettid=178) - nikdy neni 0. */
    {
        int *tid_slot = (int *)(new_tp - 0x650);   /* TP-0x720+0xD0 */
        *tid_slot = 0;
        long t = raw_syscall6(178, 0,0,0,0,0, (long)F2_SENTINEL);
        if (t > 0)
            *tid_slot = (int)t;
    }

    /* tcbhead_t at new_tp: { dtv, private } -- dtv filled below */
    *(uintptr_t *)(new_tp + 0x00) = 0;
    *(uintptr_t *)(new_tp + 0x08) = 0;

    for (size_t i = 0; i < g_tls_mod_count; i++) {
        elf_object_t *m = g_tls_mods[i];
        if (!m)
            continue;
        /* .tdata image z modulu (arena/locale pointery libc!), ne garbage
         * z host TP (na Androidu bionic layout nekompatibilní). */
        char *dst = (char *)new_tp + m->tls_offset;
        /* glibc semantika: zkopiruj filesz z .tdata, zbytek do memsz vynuluj
         * (.tbss; v obrazu modulu muze byt nesmyslna data). */
        if (m->tdata_src && m->tdata_filesz)
            memcpy(dst, m->tdata_src, m->tdata_filesz);
        if (m->tls_memsz > m->tdata_filesz)
            memset(dst + m->tdata_filesz, 0, m->tls_memsz - m->tdata_filesz);
    }

    /* rseq area (na konci TLS bloku): cpu_id=-1 (0xFF) =>
     * RSEQ_GETMEM_ONCE(cpu_id) < 0 -> glibc new threads NEnastavi
     * ATTR_FLAG_DO_RSEQ a NIKDY nezavolaji rseq syscall (293, app KILL).
     * POZOR: nesmi byt na TP+0x10 - kolidovala by s TLS blokem hlavniho exe,
     * ktery non-PIE binarky adresuji pres zapečene TPREL offsety (TP+0x10+). */
    uintptr_t rseq_off = ELF_TLS_RESERVE;
    __builtin_memset((void *)(new_tp + rseq_off), 0xff, ELF_RSEQ_SIZE);
    ldso_rseq_offset = (int64_t)rseq_off;

    /* Build a glibc-shaped DTV so __tls_get_addr (GD/LD TLS) works.
       dtv_t layout: u[0]=counter | {val,to_free}, 16 bytes per entry.
       A[0]=delka, A[1]=generace, A[2+modid-1]=entry; tcbhead.dtv = &A[1]. */
    typedef struct { uintptr_t u[2]; } ldso_dtv_t;
    ldso_dtv_t *dtv = (ldso_dtv_t *)(new_tp + rseq_off + ELF_RSEQ_SIZE);
    memset(dtv, 0, (2 + ELF_MAX_TLS_MODS + 16) * sizeof(ldso_dtv_t));
    dtv[0].u[0] = g_tls_mod_count + 1;   /* dtv length (dtv[-1].counter) */
    dtv[1].u[0] = 1;                     /* TLS generation counter */
    for (size_t i = 0; i < g_tls_mod_count; i++)
        dtv[2 + i].u[0] = new_tp + g_tls_mods[i]->tls_offset;
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

/* Dynamicky nacteny modul s PT_TLS: zkopiruj jeho .tdata do aktualniho
 * threadu (TP + tls_offset) a nastav DTV entry. Diky fixni ELF_TLS_RESERVE
 * je tls_offset vzdy uvnitr alokovaneho regionu a DTV je dimenzovane na
 * ELF_MAX_TLS_MODS, takze se nic neprekryva ani nepretece. */
void elf_tls_add_module_to_thread(elf_object_t *m) {
    if (!m || !m->has_tls || !m->tls_offset || !g_tls_new_tp)
        return;
    uintptr_t tp = g_tls_new_tp;
    char *dst = (char *)tp + m->tls_offset;
    if (m->tdata_src && m->tdata_filesz)
        memcpy(dst, m->tdata_src, m->tdata_filesz);
    if (m->tls_memsz > m->tdata_filesz)
        memset(dst + m->tdata_filesz, 0, m->tls_memsz - m->tdata_filesz);

    /* DTV: tcbhead.dtv -> &A[1]; A[0]=delka, A[1]=generace, A[2+modid-1]=val */
    typedef struct { uintptr_t u[2]; } ldso_dtv_t;
    uintptr_t rseq_off = elf_tls_rseq_offset();
    ldso_dtv_t *A = (ldso_dtv_t *)(tp + rseq_off + ELF_RSEQ_SIZE);
    size_t modid = 0;
    for (size_t i = 0; i < g_tls_mod_count; i++)
        if (g_tls_mods[i] == m) { modid = i + 1; break; }
    if (modid == 0 || modid > ELF_MAX_TLS_MODS)
        return;
    if (modid + 1 > A[0].u[0])
        A[0].u[0] = modid + 1;
    A[1 + modid].u[0] = (uintptr_t)dst;
    A[1 + modid].u[1] = 0;
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
    /* Self-test TRAP->SIGSYS handler: pokud ELF_LOADER_SIGSYS_TEST=1,
     * nainstaluj filtr, ktery setfsuid(151) vraci TRAP, a zavolej ho.
     * Pokud handler funguje, vypise se [SIGSYS] handler entered a pokracuje. */
    if (getenv("ELF_LOADER_SYSCALL_PROBE")) {
        long lo = 0, hi = 450;
        const char *rg = getenv("ELF_LOADER_PROBE_RANGE");
        if (rg) sscanf(rg, "%ld-%ld", &lo, &hi);
        for (long nr = lo; nr <= hi; nr++) {
            if (nr == 93 || nr == 94 || nr == 139 || nr == 142 || nr == 221)
                continue;  /* exit/exit_group/rt_sigreturn/reboot/execve */
            pid_t pid = fork();
            if (pid == 0) {
                alarm(1);  /* blokujici syscall (pause/poll/...) -> SIGALRM */
                syscall(nr, 0L, 0L, 0L, 0L, 0L, 0L);
                _exit(0);
            }
            int st = 0;
            if (pid > 0) {
                waitpid(pid, &st, 0);
                if (WIFSIGNALED(st)) {
                    int s = WTERMSIG(st);
                    if (s == SIGALRM)
                        fprintf(stderr, "[probe] nr=%ld HANG\n", nr);
                    else
                        fprintf(stderr, "[probe] nr=%ld KILLED signo=%d\n", nr, s);
                } else if (WIFEXITED(st) && WEXITSTATUS(st) == 159)
                    fprintf(stderr, "[probe] nr=%ld TRAP->sigsys_handler\n", nr);
            }
        }
        fprintf(stderr, "[probe] done %ld..%ld\n", lo, hi);
    }
    if (getenv("ELF_LOADER_SIGSYS_TEST")) {
        struct sock_filter tp[4];
        tp[0] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                             offsetof(struct seccomp_data, nr));
        tp[1] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 151, 0, 1);
        tp[2] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP);
        tp[3] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        struct sock_fprog tfp = { .len = 4, .filter = tp };
        syscall((long)277 /* seccomp */, 1UL, 0UL, &tfp);
        long r = syscall((long)151, 0L);
        fprintf(stderr, "[sigsys-test] setfsuid -> %ld (ocekavano >= 0)\n", r);
    }
    /* Diagnostika: kolik seccomp filtru je nasteveno (nase + app profil).
     * Pozn.: seccomp kombinuje filtry pres NEJPRIZNIVEJSI akci (KILL < TRAP
     * < ERRNO < ALLOW). Kdyz app profil vraci KILL, nase ERRNO ho neprebije
     * a SIGSYS handler se vubec nespusti. */
    {
        int fd = open("/proc/self/status", O_RDONLY);
        if (fd >= 0) {
            char b[8192];
            ssize_t n = read(fd, b, sizeof b - 1);
            close(fd);
            if (n > 0) {
                b[n] = 0;
                char *p = strstr(b, "Seccomp");
                if (p && getenv("ELF_LOADER_DIAG")) {
                    char *e = strchr(p, '\n');
                    if (e)
                        *e = 0;
                    fprintf(stderr, "[compat] %s\n", p);
                }
            }
        }
    }
}
static void install_legacy_syscall_filter_impl(void) {
    /* MINIMALNI program nejdrive — izolace EINVAL priciny */
    struct sock_filter prog[160];
    size_t n = 0;
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                            offsetof(struct seccomp_data, nr));
    /* aarch64: vsechny novejsi syscally (pidfd/io_uring/clone3/openat2/...)
     * maji cislo >= 424. Prelozit hromadne na ENOSYS; kernel 4.14 je
     * stejne nezna a app seccomp profil pro ne vraci TRAP (SIGSYS). */
    prog[n++] = (struct sock_filter)
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 424, 0, 1);
    prog[n++] = (struct sock_filter)
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
    /* Dalsi casto TRAPovane: statx=291, membarrier=283,
     * userfaultfd=282, preadv2/pwritev2/copy_file_range/pkey=285-289.
     * POZOR: getrandom(278) ZAMERNE NEblokujeme - node/OpenSSL CSPRNG
     * pres nej ziskava entropii; s ENOSYS selze ncrypto::CSPRNG assert.
     * (Rootfs nema /dev/urandom, takze fallback nefunguje.) */
    static const int blocked2[] = { 282, 283, 285, 286, 287, 288, 289, 291 };
    for (size_t i = 0; i < sizeof(blocked2) / sizeof(blocked2[0]); i++) {
        prog[n++] = (struct sock_filter)
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, blocked2[i], 0, 1);
        prog[n++] = (struct sock_filter)
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
    }
    /* aarch64 nr novejsich syscallu, ktere app seccomp profil na kernelu
     * 4.14 blokuje TRAPem (SIGSYS). Prelozime je na ENOSYS, aby glibc
     * fallbacky fungovaly:
     *   293 rseq (glibc >= 2.35 registruje v kazdem vlakne!)
     *   435 clone3, 436 close_range, 437 openat2, 439 faccessat2
     *   440 process_madvise, 441 epoll_pwait2, 449 futex_waitv
     *   282 userfaultfd, 434 pidfd_open */
    static const int blocked[] = { 293, 282, 434, 435, 436, 437, 439, 440, 441, 449, 283, 291 };
    for (size_t i = 0; i < sizeof(blocked)/sizeof(blocked[0]); i++) {
        prog[n++] = (struct sock_filter)
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, blocked[i], 0, 1);
        prog[n++] = (struct sock_filter)
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
    }
    if (getenv("ELF_LOADER_TRAP_ALL"))
        prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP);
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

/* Diagnostika: stav internich malloc/lock cache guest ld.so. Signal-safe. */
static void ldsodump_line(const char *tag, uintptr_t base, long off, uintptr_t val) {
    static const char hx[] = "0123456789abcdef";
    char buf[160];
    char *p = buf;
    char *end = buf + sizeof(buf) - 1;
    #define DP(s) do { const char *__s = (s); while (*__s && p < end) *p++ = *__s++; } while (0)
    #define DH(v) do { uintptr_t __v = (uintptr_t)(v); DP("0x"); \
        for (int __sh = 60; __sh >= 0; __sh -= 4) { if (p < end) *p++ = hx[(__v >> __sh) & 0xf]; } } while (0)
    DP("[LDSO] "); DP(tag); DP(" base="); DH(base);
    DP(" off="); DH((uintptr_t)off); DP(" val="); DH(val);
    *p++ = '\n';
    tls_raw_write(2, buf, (size_t)(p - buf));
    #undef DP
    #undef DH
}

void elf_dump_ldso_state(elf_scope_t *scope) {
    if (!scope)
        return;
    for (size_t i = 0; i < scope->count; i++) {
        elf_object_t *m = scope->mods[i];
        if (!m || !m->soname || !m->base_addr)
            continue;
        if (strncmp(m->soname, "ld-linux", 8) != 0)
            continue;
        uintptr_t b = (uintptr_t)m->base_addr;
        ldsodump_line("ro-0x68", b, 0x3fb00, *(volatile uintptr_t *)(b + 0x3fb00));
        ldsodump_line("ro+0x10", b, 0x3fb78, *(volatile uintptr_t *)(b + 0x3fb78));
        ldsodump_line("ro+0x28", b, 0x3fb90, *(volatile uintptr_t *)(b + 0x3fb90));
        ldsodump_line("g+0xb58", b, 0x40b58, *(volatile uintptr_t *)(b + 0x40b58));
        return;
    }
}

/* Dump libc GOT slotu pro _dl_allocate_tls: overi, kam se resolvoval. */
void elf_dump_tls_got(elf_scope_t *scope) {
    if (!scope)
        return;
    for (size_t i = 0; i < scope->count; i++) {
        elf_object_t *m = scope->mods[i];
        if (!m || !m->soname || !m->base_addr || !m->jmp_rela)
            continue;
        if (strncmp(m->soname, "libc.so.6", 9) != 0)
            continue;
        for (size_t off = 0; off < m->jmp_size; off += sizeof(Elf64_Rela)) {
            Elf64_Rela *r = (Elf64_Rela *)((char *)m->jmp_rela + off);
            size_t si = ELF64_R_SYM(r->r_info);
            if (si >= m->dynsym_count)
                continue;
            const char *nm = m->dynstr + m->dynsym[si].st_name;
            if (strcmp(nm, "_dl_allocate_tls") != 0)
                continue;
            uintptr_t slot = (uintptr_t)va(m, r->r_offset);
            uintptr_t val = *(volatile uintptr_t *)slot;
            ldsodump_line("libc.got.alloc_tls", (uintptr_t)m->base_addr,
                          (long)r->r_offset, val);
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
        register long x5 __asm__("x5") = (long)F2_SENTINEL;  /* bypass F2 filtru */
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x5)
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
    elf_dump_ldso_state(g_crash_scope);
    elf_dump_tls_got(g_crash_scope);

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
                elf_fault_map_one(ra);
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


/* ===== F2 seccomp path-translation (proot-lite) =====
 * Stackovany filtr (vedle elf_install_compat) zachytava openat/statx/
 * newfstatat/readlinkat/faccessat pres SECCOMP_RET_TRAP. SIGSYS handler
 * (nizze) prelozi cestu (x1) a syscall zemuluje raw svc s "SENTINEL" v x5,
 * coz filtr pousti (aby se handleruv raw syscall nezacyklil). Tim se chyti
 * i glibc-interni otevirani (IFUNC open64), ktere PLT-override nechyta ->
 * opravuje sed/sort/awk. */

static const char *g_f2_root = NULL;
static char g_f2_sc[8192];
static const char *g_f2_excl[] = {
    "/proc", "/sys", "/dev", "/system", "/apex", "/vendor",
    "/product", "/odm", "/mnt", "/metadata", NULL
};
void f2_set_root(const char *r) { g_f2_root = r; }

static size_t f2_slen(const char *s) { size_t n = 0; if (s) while (s[n]) n++; return n; }
static int f2_sncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
static void f2_scpy(char *d, const char *s) { if (s) while (*s) *d++ = *s++; *d = 0; }
static void f2_scat(char *d, const char *s) { while (*d) d++; if (s) while (*s) *d++ = *s++; *d = 0; }

/* Prelozi absolutni /X -> $ROOTFS/X (idempotentni: uz pod ROOTFS -> beze zmeny).
 * Exclude list (host fs) a relativni cesty se neprekladaji. */
static int f2_translate(const char *in, char *out, size_t outsz) {
    if (!in) { out[0] = 0; return 0; }
    if (!g_f2_root) { f2_scpy(out, in); return 0; }
    size_t rl = f2_slen(g_f2_root);
    if (rl && f2_slen(in) >= rl && f2_sncmp(in, g_f2_root, rl) == 0) {
        f2_scpy(out, in); return 1;                 /* uz pod ROOTFS -> no-op */
    }
    for (int i = 0; g_f2_excl[i]; i++) {
        size_t el = f2_slen(g_f2_excl[i]);
        if (f2_sncmp(in, g_f2_excl[i], el) == 0 && (in[el] == '/' || in[el] == 0)) {
            f2_scpy(out, in); return 0;             /* host fs -> bez prekladu */
        }
    }
    if (in[0] != '/') { f2_scpy(out, in); return 0; }   /* relativni -> bez */
    if (rl + f2_slen(in) + 1 > outsz) { f2_scpy(out, in); return 0; }
    f2_scpy(out, g_f2_root);
    f2_scat(out, in);
    return 1;
}

/* Raw syscall (aarch64): x8=nr, x0..x5=args. x5 nese SENTINEL, aby filtr
 * handleruv emulovany syscall pustil (ne znovu TRAP). */
static long raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5)
                     : "memory","cc");
    return x0;
}

/* Kernel struct stat (aarch64) pro newfstatat - st_mode je na ofsetu 16.
 * _u je velka rezerva, aby kernel (i s 64-bit casy) nepresahl buffer. */
struct f2_kstat {
    unsigned long _dev; unsigned long _ino; unsigned int _mode; unsigned int _nlink;
    unsigned int _uid; unsigned int _gid; unsigned long _rdev; unsigned long _pad1;
    long _size; int _blksize; int _pad2; long _blocks;
    long _atime; long _atime_ns; long _mtime; long _mtime_ns;
    long _ctime; long _ctime_ns; unsigned long _u[64];
};
#define F2_S_IFMT 0170000
#define F2_S_IFLNK 0120000
#define F2_AT_SYMLINK_NOFOLLOW 0x100

/* Vyresi symlinky v ceste pod ROOTFS (absolutni cile /etc/alt -> $ROOTFS/...)
 * tak, aby kernel pri otevirani/statu nedival proti realnemu rootu (to dela
 * proot pres readlinkat). Pro cesty mimo ROOTFS (exclude) / relativni -> beze zmeny. */
/* Statické buffery (mimo altstack signal-handleru) - zabrani pretizeni
 * altstacku pri rekurzivnim reseni symlinku. Handler nereentruje (SENTINEL
 * zabrani zacykleni SIGSYS), takze sdilene buffery jsou bezpecne. */
static char f2_rp_cur[8192];
static char f2_rp_next[8192];
static char f2_rp_link[8192];
#define F2_RP_SZ 8192
static void f2_realpath(const char *guest, char *out, size_t outsz) {
    (void)outsz;
    if (!guest || !guest[0]) { if (out) out[0] = 0; return; }
    for (int i = 0; g_f2_excl[i]; i++) {
        size_t el = f2_slen(g_f2_excl[i]);
        if (f2_sncmp(guest, g_f2_excl[i], el) == 0 && (guest[el] == '/' || guest[el] == 0)) {
            f2_scpy(out, guest); return;
        }
    }
    char *cur = f2_rp_cur;
    f2_translate(guest, cur, F2_RP_SZ);
    size_t rl = g_f2_root ? f2_slen(g_f2_root) : 0;
    int under = (rl && f2_slen(cur) >= rl && f2_sncmp(cur, g_f2_root, rl) == 0);
    if (!under) { f2_scpy(out, cur); return; }
    for (int depth = 0; depth < 32; depth++) {
        struct f2_kstat st;
        long r = raw_syscall6(79, (long)-100, (long)cur, (long)&st,
                              F2_AT_SYMLINK_NOFOLLOW, 0, (long)F2_SENTINEL);
        if (r != 0 || !((st._mode & F2_S_IFMT) == F2_S_IFLNK)) {
            f2_scpy(out, cur); return;
        }
        char *linkbuf = f2_rp_link;
        long lr = raw_syscall6(78, (long)-100, (long)cur, (long)linkbuf,
                               (long)(F2_RP_SZ - 1), 0, (long)F2_SENTINEL);
        if (lr < 0) { f2_scpy(out, cur); return; }
        linkbuf[lr] = 0;
        char *next = f2_rp_next;
        if (linkbuf[0] == '/') {
            f2_translate(linkbuf, next, F2_RP_SZ);
        } else {
            f2_scpy(next, cur);
            char *sl = next + f2_slen(next);
            while (sl > next + 1 && sl[-1] != '/') sl--;
            *sl = 0;
            f2_scat(next, linkbuf);
        }
        if (f2_slen(next) >= F2_RP_SZ - 1) { f2_scpy(out, next); return; }
        f2_scpy(cur, next);
    }
    f2_scpy(out, cur);
}

/* Filtr: pro path-syscally (openat/statx/newfstatat/readlinkat/faccessat)
 * vraci TRAP, POKUD x5 (args[5]) != SENTINEL. SENTINEL v x5 = handlerova
 * emulace -> ALLOW (zabrani zacykleni). Ostatni syscally -> ALLOW. */
static void install_f2_path_filter_impl(void) {
    struct sock_filter prog[64];
    size_t n = 0;
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                            offsetof(struct seccomp_data, nr));
    /* aarch64 cisla: openat=56 statx=291 newfstatat=79 readlinkat=78 faccessat=48
     * (POZOR: 63=read, 65=readv -> nesmi byt v seznamu!) */
    /* Jen openat: staci pro DNS (/etc/resolv.conf) i vetsinu path prekladu.
     * statx/newfstatat/faccessat ZAMERNE vynechany - TRAP na ne rozbiji
     * glibc interni cesty (NSS/dl_iterate_phdr) -> segfault v id/whoami. */
    static const int pathnrs[] = { 56 };
    for (size_t i = 0; i < sizeof(pathnrs)/sizeof(pathnrs[0]); i++) {
        prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                                 pathnrs[i], 0, 6);
        prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                       offsetof(struct seccomp_data, args[5]) + 0);
        prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                       (unsigned)(F2_SENTINEL & 0xffffffffUL), 0, 3);
        prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                       offsetof(struct seccomp_data, args[5]) + 4);
        prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                       (unsigned)(F2_SENTINEL >> 32), 0, 1);
        prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP);
        prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                       offsetof(struct seccomp_data, nr));
    }
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    struct sock_fprog fprog = { .len = (unsigned short)n, .filter = prog };
    long rp = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    long rs = syscall((long)277, 1UL, 0UL, &fprog);
    if (rs == 0) g_f2_filter_active = 1;
    /* diag: zapsat navratove kody (raw, TP-independent) */
    {
        int fd = (int)raw_syscall6(56, -100L,
            (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt",
            0x441L, 0644L, 0, (long)F2_SENTINEL);
        if (fd >= 0) {
            char b[96]; int i = 0;
            const char *p = "FILTER prctl=";
            while (*p) b[i++] = *p++;
            if (rp < 0) { b[i++]='-'; rp = -rp; }
            char t[20]; int ti = 0;
            if (rp == 0) t[ti++]='0';
            while (rp > 0) { t[ti++] = '0' + (rp % 10); rp /= 10; }
            while (ti > 0) b[i++] = t[--ti];
            p = " seccomp=";
            while (*p) b[i++] = *p++;
            if (rs < 0) { b[i++]='-'; rs = -rs; }
            ti = 0;
            if (rs == 0) t[ti++]='0';
            while (rs > 0) { t[ti++] = '0' + (rs % 10); rs /= 10; }
            while (ti > 0) b[i++] = t[--ti];
            b[i++] = '\n';
            raw_syscall6(64, fd, (long)(unsigned long)b, i, 0, 0, (long)F2_SENTINEL);
            raw_syscall6(57, fd, 0, 0, 0, 0, (long)F2_SENTINEL);
        }
    }
}
void install_f2_path_filter(void) {
    if (g_f2_root) install_f2_path_filter_impl();
}

/* SIGSYS = seccomp odmitl syscall. Vypise cislo syscallu cistym sys_write
 * (handler muze bezet pod parrot TP, bionic stdio je tam nedostupne). */
static void sigsys_handler(int, siginfo_t *, void *);

/* Reinstalace SIGSYS handleru KERNEL layoutem (bionic struct sigaction ma
 * jiny layout nez kernel!). Volej z handleru, aby nas handler "drzel" i kdyz
 * ho guest runtime (glibc/zsh) prebije. raw syscall = TP-independent. */
static void f2_reinstall_sigsys(void) {
    struct {
        void (*h)(int, void *, void *);
        unsigned long flags;
        void (*r)(void);
        unsigned long mask;
    } ka;
    ka.h = (void (*)(int, void *, void *))sigsys_handler;
    ka.flags = 4UL;   /* SA_SIGINFO */
    ka.r = 0;
    ka.mask = 0;
    raw_syscall6(134, 31L /* SIGSYS */, (long)(unsigned long)&ka, 0L, 8L, 0L,
                 (long)F2_SENTINEL);
}

static void sigsys_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig;
    { static const char _m[] = "HANDLER-ENTER\n"; int _fd = raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x441L, 0644L, 0, (long)F2_SENTINEL); if (_fd >= 0) { raw_syscall6(64, _fd, (long)(unsigned long)_m, sizeof(_m) - 1, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); } }
    if (g_tls_trace) {
        static const char _m[] = "[SIGSYS] handler entered\n";
        raw_syscall6(64, 2, (long)(unsigned long)_m, sizeof(_m) - 1, 0, 0,
                     (long)F2_SENTINEL);
    }
    { static const char _m[] = "ENTER\n"; int _fd = raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x241L, 0644L, 0, (long)F2_SENTINEL); if (_fd >= 0) { raw_syscall6(64, _fd, (long)(unsigned long)_m, 6, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); } }
    ucontext_t *ctx = (ucontext_t *)uc;
    f2_reinstall_sigsys();   /* drz nas handler i kdyz ho guest resetuje */
    long nr = si->si_syscall;
    { char _b[32]; int _i = 0; const char *_p = "SIGSYS nr=";
      while (*_p) _b[_i++] = *_p++;
      unsigned long _n = (unsigned long)nr; char _t[24]; int _ti = 0;
      if (_n == 0) _t[_ti++] = '0';
      while (_n > 0) { _t[_ti++] = (char)('0' + (_n % 10)); _n /= 10; }
      while (_ti > 0) _b[_i++] = _t[--_ti];
      _b[_i++] = '\n';
      int _fd = (int)raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x441L, 0644L, 0, (long)F2_SENTINEL);
      if (_fd >= 0) { raw_syscall6(64, _fd, (long)(unsigned long)_b, _i, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); } }
    /* F2 seccomp path-translation: prelozime cestu (x1) a zemulujeme syscall
     * raw svc s SENTINEL v x5 (filtr ho pusti). Chyti i glibc-interni open. */
    /* stejna sada jako install_f2_path_filter_impl: openat=56 statx=291
     * newfstatat=79 readlinkat=78 faccessat=48 */
    if (nr == 56 && g_f2_root) {
        const char *orig = (const char *)(unsigned long)ctx->uc_mcontext.regs[1];
        long dirfd = (long)ctx->uc_mcontext.regs[0];
        long a2 = (long)ctx->uc_mcontext.regs[2];
        long a3 = (long)ctx->uc_mcontext.regs[3];
        long a4 = (long)ctx->uc_mcontext.regs[4];
        /* Preklad jen pro /etc/resolv.conf (DNS). Ostatni cesty posli na
         * kernel BEZE ZMENY - TRAP+emulace vsech openatu rozbiji glibc
         * interni cesty (NSS/dlopen/dl_iterate_phdr) -> segfault v id/zsh. */
        static const char rc[] = "/etc/resolv.conf";
        int is_rc = 0;
        if (orig) {
            is_rc = 1;
            for (int i = 0; rc[i]; i++) {
                if (orig[i] != rc[i]) { is_rc = 0; break; }
            }
        }
        char tr[4096];
        const char *use = orig;
        if (is_rc && orig && orig[0] == '/') {
            f2_translate(orig, tr, sizeof tr);
            use = tr;
        }
        long r = raw_syscall6(56, dirfd, (long)(unsigned long)use, a2, a3, a4,
                              (long)F2_SENTINEL);
        ctx->uc_mcontext.regs[0] = r;
        return;
    }
    /* Emulovatelné syscally zakazané Androidím seccompem.
     * Handler se spusti POUZE pri SIGSYS = syscall zablokovaný seccomp filtrem;
     * povolené syscally jdou na reálné jádro a sem vubec nedojdou.
     * Vracíme benigní hodnotu a preskocíme svc #0 -> binárka pokracuje.
     * (futex_waitv 444 apod. vyžadují reálné jádro -> neemulovatelné.) */
    /* rt_sigaction(134) se SIGSYS: cizi pokus prepsat nas handler ->
     * predstirej uspech, NAS handler zustava aktivni. */
    if (nr == 134) {
        long _old = (long)ctx->uc_mcontext.regs[2];
        if (_old)
            raw_syscall6(134, 31L, 0L, _old, 8L, 0L, (long)F2_SENTINEL);
        ctx->uc_mcontext.regs[0] = 0;
        return;
    }
    /* accept(202) je app seccompem TRAPnuta (overeno: DENIED nr=202 pri
     * faked-tcp connectu) -> faked-tcp umira. Prelozime na accept4(242)
     * (aarch64 glibc accept == accept4 s flags=0). Kdyby i 242 byla TRAP,
     * handler se re-enters s nr=242 a vrati -ENOSYS (zadna nekonecna smycka). */
    if (nr == 202) {
        long fd  = (long)ctx->uc_mcontext.regs[0];
        long sa  = (long)ctx->uc_mcontext.regs[1];
        long len = (long)ctx->uc_mcontext.regs[2];
        long r = raw_syscall6(242, fd, sa, len, 0, 0, (long)F2_SENTINEL);
        ctx->uc_mcontext.regs[0] = r;
        f2_reinstall_sigsys();
        return;
    }
    long emu = -999;  /* -999 = nemáme emulaci pro tento nr */
    switch (nr) {
        /* POZOR: getuid()/getgid() jsou BIONICKE funkce - pod parrot TP
         * (kde handler bezi) ctou spatny TLS/errno slot -> crash. Proto RAW
         * syscall (aarch64: getuid=174, getgid=176) se SENTINELem. */
        case 151: emu = raw_syscall6(174, 0,0,0,0,0, (long)F2_SENTINEL); break; /* setfsuid -> getuid */
        case 152: emu = raw_syscall6(176, 0,0,0,0,0, (long)F2_SENTINEL); break; /* setfsgid -> getgid */
        case 140: emu = 0; break;         /* setpriority (best-effort) */
        case 141: emu = 0; break;         /* getpriority (best-effort) */
        case 235: emu = 0; break;         /* mbind */
        case 237: emu = 0; break;         /* set_mempolicy */
        case 238: emu = 0; break;         /* migrate_pages */
        case 239: emu = 0; break;         /* move_pages */
        case 217: emu = -38; break;       /* add_key        -> -ENOSYS */
        case 218: emu = -38; break;       /* request_key    -> -ENOSYS */
        case 219: emu = -38; break;       /* keyctl         -> -ENOSYS */
        case 236: emu = -38; break;       /* get_mempolicy  -> -ENOSYS */
        case 116: emu = -38; break;       /* syslog (dmesg) -> -ENOSYS */
        case 264: emu = -38; break;       /* name_to_handle_at -> -ENOSYS */
        case 439: emu = -38; break;       /* faccessat2 (systemd) -> -ENOSYS */
        case 99:  emu = -38; break;       /* set_robust_list -> -ENOSYS (app profil ho KILLuje) */
        case 293: emu = -38; break;       /* rseq -> -ENOSYS (glibc 2.35+ ho registruje) */
        case 282: emu = -38; break;       /* userfaultfd -> -ENOSYS */
        case 434: emu = -38; break;       /* pidfd_open -> -ENOSYS */
        case 440: emu = -38; break;       /* process_madvise -> -ENOSYS */
        case 441: emu = -38; break;       /* epoll_pwait2 -> -ENOSYS */
        case 449: emu = -38; break;       /* futex_waitv -> -ENOSYS */
        case 435: emu = -38; break;       /* clone3 -> -ENOSYS (glibc falls back to clone) */
        case 436: emu = -38; break;       /* close_range -> -ENOSYS */
        case 437: emu = -38; break;       /* openat2 -> -ENOSYS (glibc falls back to openat) */
        case 180 ... 185: emu = -38; break;  /* mq_* (POSIX queues) -> -ENOSYS */
        case 186 ... 189: emu = -38; break;  /* msg* (SysV) -> -ENOSYS */
        case 190 ... 193: emu = -38; break;  /* sem* (SysV) -> -ENOSYS */
        case 194: case 195: case 198: case 199: emu = -38; break; /* shm* (SysV) -> -ENOSYS */
        default: break;
    }
    if (emu != -999) {
        /* POZOR: na tomto arm64 jiz ucontext.pc ukazuje na svc+4
         * (viz F2 vetev vyse), takze pc NEMENIME - jinak bychom
         * preskocili dalsi instrukci a spadli. */
        ctx->uc_mcontext.regs[0] = emu;       /* emulovaná navratová hodnota */
        f2_reinstall_sigsys();                /* drz nas handler */
        return;                               /* sigreturn -> pokracovani */
    }
    /* DIAG: zaloguj cislo neemulovaneho syscallu a vrat -ENOSYS misto
     * _exit(159) - proces tak pokracuje a my zjistime, co zsh -i potrebuje. */
    {
        char buf[48];
        const char *pre = "DENIED nr=";
        char *p = buf;
        for (const char *q = pre; *q; q++) *p++ = *q;
        char tmp[24]; int ti = 0;
        unsigned long _n = nr;
        if (_n == 0) tmp[ti++] = '0';
        while (_n > 0) { tmp[ti++] = (char)('0' + (_n % 10)); _n /= 10; }
        while (ti > 0) *p++ = tmp[--ti];
        *p++ = '\n';
        int _fd = (int)raw_syscall6(56, -100L,
            (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt",
            0x441L, 0644L, 0, (long)F2_SENTINEL);
        if (_fd >= 0) {
            raw_syscall6(64, _fd, (long)(unsigned long)buf, (size_t)(p - buf),
                         0, 0, (long)F2_SENTINEL);
            raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL);
        }
        ctx->uc_mcontext.pc += 4;             /* preskoc svc #0 */
        ctx->uc_mcontext.regs[0] = (u_int64_t)-38L;  /* -ENOSYS */
        return;
    }
}

/* Early SIGSYS handler installation via constructor: after re-exec the child
 * inherits seccomp filters but SIGSYS is SIG_DFL. The constructor runs before
 * main(), closing the window where bionic ld.so or early init could hit a
 * TRAP'd syscall and die. Only installs SIGSYS (not SIGSEGV/SIGILL/SIGBUS)
 * to avoid interfering with linker fault handling. */
static void install_sigsys_handler_now(void) {
    f2_reinstall_sigsys();
}

__attribute__((constructor(101)))
static void early_install_sigsys(void) {
    static char early_altstack[262144];
    static stack_t ess;
    ess.ss_sp = early_altstack;
    ess.ss_size = sizeof(early_altstack);
    sigaltstack(&ess, NULL);
    install_sigsys_handler_now();
}

void elf_install_fault_handlers(void) {
    static char altstack[262144];  /* dost na SIGSYS handler + f2_realpath */
    static stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, NULL);
    /* SIGSYS: vypise cislo odmitaneho syscallu a exit(159) */
    struct sigaction sc;
    memset(&sc, 0, sizeof(sc));
    /* DEBUG: potvrdit, ze handler byl nainstalovan - zapis do souboru */
    { int _fd = raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x241L, 0644L, 0, (long)F2_SENTINEL); if (_fd >= 0) { const char _m[] = "INSTALLED\n"; raw_syscall6(64, _fd, (long)(unsigned long)_m, 10, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); } }
    sc.sa_sigaction = sigsys_handler;
    sc.sa_flags = SA_SIGINFO;
    f2_reinstall_sigsys();
    if (getenv("ELF_LOADER_DIAG")) {
        struct sigaction chk;
        memset(&chk, 0, sizeof chk);
        if (sigaction(SIGSYS, NULL, &chk) == 0)
            fprintf(stderr, "[sig] SIGSYS cur=%p want=%p flags=0x%x\n",
                    (void *)chk.sa_sigaction, (void *)sigsys_handler,
                    (unsigned)chk.sa_flags);
    }
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

/* Otestuje hypotezu "handler je prepsan": zablokuje rt_sigaction(SIGSYS)
 * na urovni jadra (EPERM), takze nam SIGSYS handler NIKDO nemuze prepsat.
 * Kdyz starship i potom spadne na SIGSYS, je to SECCOMP KILL/TRAP z app
 * profilu, ktery obchazi handler (KILL) - ne prepsani handleru. */
static void elf_install_sigsys_lock(void) {
    struct sock_filter prog[8];
    size_t n = 0;
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                            offsetof(struct seccomp_data, nr));
    prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                            134 /* rt_sigaction aarch64 */, 0, 4);
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                            offsetof(struct seccomp_data, args[0]));
    prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 31 /* SIGSYS */, 0, 1);
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    struct sock_fprog fprog = { .len = (unsigned short)n, .filter = prog };
    long r = syscall((long)277, 1UL, 0UL, &fprog);
    { int _fd = raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x441L, 0644L, 0, (long)F2_SENTINEL); if (_fd >= 0) { const char _m[] = "SIGSYS-LOCK\n"; raw_syscall6(64, _fd, (long)(unsigned long)_m, sizeof(_m)-1, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); } }
    (void)r;
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
    /* POZOR: po execve je SIGSYS handler resetovan na SIG_DFL. Pokud
     * target/binarka re-execuje loader (shim_execve), nova instance dostane
     * zděděny seccomp F2 filtr, ale SIGSYS handler je SIG_DFL az do main().
     * Bionic ld.so pri startu muze delat openat/newfstatat -> padne na SIGSYS
     * drive nez main() doběhne a reinstaluje handler. Reinstalace ZDE pred
     * jump_to_entry garantuje ze handler je aktivni prave kdyz guest code
     * zaclani behat. */
    elf_install_fault_handlers();
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
    if (getenv("ELF_LOADER_KEEP_HANDLERS"))
        elf_install_fault_handlers();
    if (getenv("ELF_LOADER_DEBUG_PC"))
        elf_install_debug_sigaction_block();
    if (getenv("ELF_LOADER_SIGSYS_LOCK"))
        elf_install_sigsys_lock();
    fflush(stderr);
    { int _fd = raw_syscall6(56, (long)0xFFFFFFFFFFFFFF9CL, (long)(unsigned long)"/data/user/0/com.linux_core/files/usr/diag.txt", 0x441L /*O_WRONLY|O_CREAT|O_APPEND*/, 0644L, 0, (long)F2_SENTINEL); if (_fd >= 0) { const char _m[] = "JUMP\n"; raw_syscall6(64, _fd, (long)(unsigned long)_m, 5, 0, 0, (long)F2_SENTINEL); raw_syscall6(57, _fd, 0, 0, 0, 0, (long)F2_SENTINEL); } }
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