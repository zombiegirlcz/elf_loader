#include <stdio.h>
#include <stddef.h>
#include <link.h>
#include <dlfcn.h>

/* These are internal glibc structures - we define them based on glibc 2.41 source */
struct dl_namespace {
    struct link_map *_ns_loaded;
    unsigned int _ns_nloaded;
    struct link_map *_ns_main_searchlist;
    struct link_map *_ns_global_scope[16];
    unsigned int _ns_global_scope_alloc;
    struct link_map *_ns_init_list[32];
    struct link_map *_ns_fini_list[32];
    unsigned int _ns_ninit;
    unsigned int _ns_nfini;
    /* ... more fields ... */
};

struct rtld_global {
    struct r_debug _dl_debug;
    struct link_map *_dl_loaded;      /* deprecated alias */
    struct link_map *_dl_lmwait;
    int _dl_nns;
    struct dl_namespace _dl_ns[16];   /* max namespaces */
    int _dl_non_dynamic_init;
    int dl_load_adds;
    void *dl_load_write_lock;         /* actually rtld_lock_define_recursive_t */
    /* ... many more fields ... */
};

/* External symbols from libc */
extern struct rtld_global _rtld_global;
extern struct r_debug _r_debug;

int main() {
    printf("=== glibc 2.41 Internal Structure Offsets ===\n\n");
    
    printf("sizeof(struct link_map) = %zu\n", sizeof(struct link_map));
    printf("sizeof(struct r_debug) = %zu\n", sizeof(struct r_debug));
    printf("sizeof(struct dl_namespace) = %zu\n", sizeof(struct dl_namespace));
    printf("sizeof(struct rtld_global) = %zu\n", sizeof(struct rtld_global));
    printf("\n");
    
    printf("struct link_map offsets:\n");
    printf("  l_addr       = %zu (0x%zx)\n", offsetof(struct link_map, l_addr), offsetof(struct link_map, l_addr));
    printf("  l_name       = %zu (0x%zx)\n", offsetof(struct link_map, l_name), offsetof(struct link_map, l_name));
    printf("  l_ld         = %zu (0x%zx)\n", offsetof(struct link_map, l_ld), offsetof(struct link_map, l_ld));
    printf("  l_next       = %zu (0x%zx)\n", offsetof(struct link_map, l_next), offsetof(struct link_map, l_next));
    printf("  l_prev       = %zu (0x%zx)\n", offsetof(struct link_map, l_prev), offsetof(struct link_map, l_prev));
    printf("\n");
    
    printf("struct r_debug offsets:\n");
    printf("  r_version    = %zu (0x%zx)\n", offsetof(struct r_debug, r_version), offsetof(struct r_debug, r_version));
    printf("  r_map        = %zu (0x%zx)\n", offsetof(struct r_debug, r_map), offsetof(struct r_debug, r_map));
    printf("  r_brk        = %zu (0x%zx)\n", offsetof(struct r_debug, r_brk), offsetof(struct r_debug, r_brk));
    printf("  r_state      = %zu (0x%zx)\n", offsetof(struct r_debug, r_state), offsetof(struct r_debug, r_state));
    printf("  r_ldbase     = %zu (0x%zx)\n", offsetof(struct r_debug, r_ldbase), offsetof(struct r_debug, r_ldbase));
    printf("\n");
    
    printf("struct dl_namespace offsets:\n");
    printf("  _ns_loaded   = %zu (0x%zx)\n", offsetof(struct dl_namespace, _ns_loaded), offsetof(struct dl_namespace, _ns_loaded));
    printf("  _ns_nloaded  = %zu (0x%zx)\n", offsetof(struct dl_namespace, _ns_nloaded), offsetof(struct dl_namespace, _ns_nloaded));
    printf("\n");
    
    printf("struct rtld_global offsets:\n");
    printf("  _dl_debug    = %zu (0x%zx)\n", offsetof(struct rtld_global, _dl_debug), offsetof(struct rtld_global, _dl_debug));
    printf("  _dl_loaded   = %zu (0x%zx)\n", offsetof(struct rtld_global, _dl_loaded), offsetof(struct rtld_global, _dl_loaded));
    printf("  _dl_lmwait   = %zu (0x%zx)\n", offsetof(struct rtld_global, _dl_lmwait), offsetof(struct rtld_global, _dl_lmwait));
    printf("  _dl_nns      = %zu (0x%zx)\n", offsetof(struct rtld_global, _dl_nns), offsetof(struct rtld_global, _dl_nns));
    printf("  _dl_ns[0]    = %zu (0x%zx)\n", offsetof(struct rtld_global, _dl_ns), offsetof(struct rtld_global, _dl_ns));
    printf("  _dl_ns[0]._ns_loaded = %zu (0x%zx)\n", 
           offsetof(struct rtld_global, _dl_ns) + offsetof(struct dl_namespace, _ns_loaded),
           offsetof(struct rtld_global, _dl_ns) + offsetof(struct dl_namespace, _ns_loaded));
    printf("  _dl_ns[0]._ns_nloaded = %zu (0x%zx)\n", 
           offsetof(struct rtld_global, _dl_ns) + offsetof(struct dl_namespace, _ns_nloaded),
           offsetof(struct rtld_global, _dl_ns) + offsetof(struct dl_namespace, _ns_nloaded));
    printf("  dl_load_adds = %zu (0x%zx)\n", offsetof(struct rtld_global, dl_load_adds), offsetof(struct rtld_global, dl_load_adds));
    printf("  dl_load_write_lock = %zu (0x%zx)\n", offsetof(struct rtld_global, dl_load_write_lock), offsetof(struct rtld_global, dl_load_write_lock));
    printf("\n");
    
    /* Try to access actual symbols from libc */
    void *handle = dlopen("libc.so.6", RTLD_LAZY);
    if (handle) {
        struct rtld_global *rg = dlsym(handle, "_rtld_global");
        struct r_debug *rd = dlsym(handle, "_r_debug");
        printf("Actual symbols from libc:\n");
        printf("  _rtld_global = %p\n", rg);
        printf("  _r_debug     = %p\n", rd);
        if (rg) {
            printf("  _dl_nns            = %d\n", rg->_dl_nns);
            printf("  _dl_ns[0]._ns_loaded = %p\n", rg->_dl_ns[0]._ns_loaded);
            printf("  _dl_ns[0]._ns_nloaded = %u\n", rg->_dl_ns[0]._ns_nloaded);
            printf("  dl_load_adds       = %d\n", rg->dl_load_adds);
        }
        dlclose(handle);
    } else {
        printf("dlopen failed: %s\n", dlerror());
    }
    
    return 0;
}