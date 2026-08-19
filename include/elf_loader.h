#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

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
elf_tls_ctx_t elf_setup_own_tls(elf_object_t *exe, elf_scope_t *scope);
void elf_teardown_own_tls(elf_tls_ctx_t *ctx);

void elf_register_override(const char *name, void *fn);
void elf_set_lazy(int on);
void *elf_lazy_resolve(uintptr_t got_slot);

extern int elf_own_deps;
extern elf_scope_t *elf_own_scope;

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

#endif