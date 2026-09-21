#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include <elf.h>
#include <signal.h>

/* NDK <elf.h> R_AARCH64_COPY nedefinuje (jen P32 variantu). */
#ifndef R_AARCH64_COPY
#define R_AARCH64_COPY 1024
#endif

/* TLS layout (aarch64, TLS_DTV_AT_TP):
 *   struct pthread  [TP-0x720, TP)
 *   tcbhead_t       { dtv, private }  na TP (16 B)
 *   TLS bloky       [TP + ELF_TLS_TCB_SIZE, ...)  <- hlavni exe MUSI byt na 0x10!
 *   rseq area       na konci vsech bloku (0xFF => cpu_id=-1)
 *
 * KRITICKE: non-PIE binarky (python/perl/...) maji local-exec TPREL offsety
 * zapečene linkerem jako TP + sym_off + TLS_TCB_SIZE (0x10). Kdyby exe blok
 * nezacinal na 0x10, binarka cte cizi data (napr. rseq area) -> fatal error.
 * Proto rseq NESMI byt na TP+0x10, ale az za vsemi TLS bloky (jako glibc
 * "extra TLS block" v _dl_determine_tlsoffset). */
#define ELF_TLS_TCB_SIZE 0x10u
#define ELF_RSEQ_SIZE    0x20u
/* Fixni rezerva pro TLS bloky vsech modulu (od TP+0x10 do TP+ELF_TLS_RESERVE).
 * rseq area + DTV jdou na PEVNY offset za ni, aby dynamicky nacitane moduly
 * (Python extension moduly jako _cffi_backend) nikdy nekolidovaly s rseq/DTV
 * a nemuseli realokovat region za behu. */
#define ELF_TLS_RESERVE  0x100000u  /* 1 MB pro TLS vsech modulu */

typedef struct {
    void *base_addr;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    int phdr_count;
    Elf64_Sym *symtab;
    char *strtab;
    size_t symtab_count;
    Elf64_Sym *dynsym;
    char *dynstr;
    size_t dynsym_count;
    void *entry_point;
    size_t total_size;

    void **handles;
    size_t handle_count;

    char *origin_dir;
    char *soname;

    Elf64_Rela *jmp_rela;
    size_t jmp_size;

    uintptr_t tls_offset;
    int has_tls;
    size_t tls_memsz;
    void *tdata_src;      /* inicializační image .tdata (ELF), ne host TLS */
    size_t tdata_filesz;  /* platná část tdata_src (zbytek do tls_memsz = 0) */

    int relocated;
    struct elf_scope *scope;
} elf_object_t;

typedef struct {
    void *region;
    size_t size;
    uintptr_t old_tp;
} elf_tls_ctx_t;

typedef struct elf_scope {
    elf_object_t **mods;
    size_t count;
    size_t cap;
    /* Hlavní exe. Není v mods (aby ho elf_scope_destroy neuvolnil dvakrát),
     * ale při hledání symbolů slouží jako fallback: Pythoní extension moduly
     * (_ctypes.so) importují PyExc_*, PyTuple_Type, _PyRuntime z hlavního
     * executable, který v mods chybí. */
    elf_object_t *exe;
} elf_scope_t;

typedef enum {
    SYM_NOT_FOUND = 0,
    SYM_DEFINED,
    SYM_IMPORT
} sym_status_t;

elf_object_t *elf_load(const char *path);
sym_status_t elf_resolve_symbol(elf_object_t *obj, const char *name, void **out_addr);
void *elf_resolve_import(elf_object_t *obj, const char *name);
int elf_relocate(elf_object_t *obj);
int elf_run(elf_object_t *obj, int argc, char **argv, char **envp);
void elf_unload(elf_object_t *obj);

void elf_install_fault_handlers(void);
void elf_set_guest_fatal(int sig, const struct sigaction *sa);
const void *elf_get_guest_fatal(int sig);
void elf_install_compat(void);
void f2_set_root(const char *r);          /* F2: nastav ROOTFS pro seccomp path-translaci */
void install_f2_path_filter(void);     /* F2: stackovany RET_TRAP filtr pro openat/statx/... */
elf_tls_ctx_t elf_setup_own_tls(elf_object_t *exe, elf_scope_t *scope);
void elf_teardown_own_tls(elf_tls_ctx_t *ctx);
void ldso_install_exe_linkmap(elf_object_t *exe, const char *name);
void ldso_install_module_list(elf_object_t *const *mods, size_t count);

void elf_register_override(const char *name, void *fn);

/* Aplikacni Android seccomp vraci SECCOMP_RET_KILL pro nektere syscally
 * (rseq=293, set_robust_list=99 s velikosti 24, clone3=435). KILL ma nejvyssi
 * precedenci a obchazi SIGSYS handler, takze nas filtr/handler ho neprebiji.
 * Obrana: v guest modulu najdi `mov x8,#nr ; ... ; svc #0` a svc prebij:
 *   err == 0 -> NOP (syscall se neprovede)
 *   err  > 0 -> `movn x0,#err-1` (volajici dostane -errno, napr. -ENOSYS)
 * U clone3 potrebujeme -ENOSYS, aby Rust/glibc fallbacknul na clone(). */
void elf_patch_syscall_sites(elf_object_t *m, long nr, long err);

/* ldso_tls.c: vlastní implementace glibc ld.so TLS funkcí (guest ld.so je
 * neinicializovaný, takže jeho _dl_allocate_tls padá na NULL _rtld_global). */
void *ldso_allocate_tls(void *mem);
void *ldso_allocate_tls_init(void *result, int main_thread);
void ldso_deallocate_tls(void *tcb, int dealloc_tcb);

/* Náhrady guest dl* nad scopes (guest glibc dlopen/dlsym padá, protože
 * nespouštíme její _dl_start -> _rtld_global je nulový). */
void *ldso_dlopen(const char *file, int mode);
void *ldso_dlsym(void *handle, const char *name);
int ldso_dlclose(void *h);
int ldso_dladdr(const void *addr, void *info_out);
const char *ldso_dlerror(void);
/* Zaregistruj novy dynamicky nacteny TLS modul do TLS aktualniho vlakna
 * (zkopiruje .tdata do TP + tls_offset a nastavi DTV entry). */
void elf_tls_add_module_to_thread(elf_object_t *m);
void elf_set_lazy(int on);
void *elf_lazy_resolve(uintptr_t got_slot);

extern int elf_own_deps;
extern elf_scope_t *elf_own_scope;

/* TLS resolution tracing (ELF_LOADER_TLS_TRACE=1) + stav cache guest ld.so. */
extern int g_tls_trace;
void elf_dump_ldso_state(elf_scope_t *s);
void elf_dump_tls_got(elf_scope_t *s);

/* Staticky TLS registry (aarch64 TLS_DTV_AT_TP). Kazdy modul s PT_TLS dostane
 * maly kladny offset od thread pointeru; ldso_tls.c pak pro novy thread
 * inicializuje DTV + zkopiruje .tdata na TP+offset. */
size_t elf_tls_module_count(void);
elf_object_t *elf_tls_module_at(size_t i);
uintptr_t elf_tls_span(void);
uintptr_t elf_tls_static_size(void);
uintptr_t elf_tls_rseq_offset(void);

extern int elf_init_argc;
extern char **elf_init_argv;
extern char **elf_init_envp;

uintptr_t elf_read_tp(void);
extern uintptr_t g_libc_base;
extern uintptr_t g_exe_base;

elf_scope_t *elf_scope_create(void);
void elf_scope_destroy(elf_scope_t *s);
void elf_scope_add(elf_scope_t *s, elf_object_t *m);
void *elf_scope_lookup(const elf_scope_t *s, const char *name);
elf_object_t *elf_scope_find(const elf_scope_t *s, const char *name,
                             const Elf64_Sym **out_sym);
elf_object_t *elf_load_shared(const char *path, elf_scope_t *scope);
/* Zaradi DT_INIT + init_array modulu (vcetne hlavniho exe) do fronty,
 * kterou spusti elf_run_final() pod parrot TP. Pro hlavni exe se musi
 * zavolat po elf_relocate(obj) - elf_load() sam inity nequeueuje. */
void elf_queue_module_inits(elf_object_t *m);

/* Debug levels */
#define ELF_DEBUG_LEVEL_NONE    0
#define ELF_DEBUG_LEVEL_ERROR   1
#define ELF_DEBUG_LEVEL_WARN    2
#define ELF_DEBUG_LEVEL_INFO    3
#define ELF_DEBUG_LEVEL_DEBUG   4
#define ELF_DEBUG_LEVEL_VERBOSE 5

/* Debug functions */
void elf_debug_init(void);
void elf_debug_log(const char *fmt, ...);
void elf_debug_trace(const char *fmt, ...);
void elf_debug_dump_maps(void);
void elf_debug_dump_symbols(elf_object_t *obj);
void elf_debug_dump_relocations(elf_object_t *obj);
void elf_debug_dump_memory(void *addr, size_t len);
void elf_debug_set_level(int level);
int elf_debug_get_level(void);

#endif