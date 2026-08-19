#include "../include/elf_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

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

static int (*real_puts)(const char *) = NULL;

static int shim_puts(const char *s) {
    fprintf(stderr, "[shim] puts(\"%s\") intercepted\n", s);
    if (!real_puts)
        real_puts = (int (*)(const char *))dlsym(RTLD_NEXT, "puts");
    return real_puts(s);
}

static int run_shim(const char *path, int argc, char **argv, char **envp) {
    elf_register_override("puts", (void *)shim_puts);
    fprintf(stderr, "[+] interposing 'puts' for %s\n", path);
    return run(path, argc, argv, envp);
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
    elf_init_argc = argc;
    elf_init_argv = argv;
    elf_init_envp = envp;

    elf_object_t *obj = elf_load(path);
    elf_own_scope = NULL;
    if (!obj) {
        elf_scope_destroy(scope);
        return 1;
    }
    printf("[+] own deps: %zu modules in scope\n", scope->count);

    if (elf_relocate(obj) != 0) {
        fprintf(stderr, "[-] Relocation failed\n");
        elf_scope_destroy(scope);
        obj->scope = NULL;
        elf_unload(obj);
        return 1;
    }

    void *libc_obj = NULL;
    for (size_t mi = 0; mi < scope->count; mi++)
        if (scope->mods[mi]->soname && strstr(scope->mods[mi]->soname, "libc.so.6"))
            libc_obj = scope->mods[mi]->base_addr;
    g_libc_base = (uintptr_t)libc_obj;
    g_exe_base = (uintptr_t)obj->base_addr;
    elf_install_fault_handlers();
    elf_tls_ctx_t tls = elf_setup_own_tls(obj, scope);
    int ret = elf_run(obj, argc, argv, envp);
    elf_teardown_own_tls(&tls);
    elf_scope_destroy(scope);
    obj->scope = NULL;
    elf_unload(obj);
    return ret;
}

int main(int argc, char **argv, char **envp) {
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