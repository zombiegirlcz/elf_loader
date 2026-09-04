# Runtime Patch Parrot libc `_rtld_global` — Poslední možnost

> **⚠️ Toto je DOKUMENTACE NÁVRHU, NE IMPLEMENTACE.** 
> Předpokládá se, že předchozí opravy (link_map linking, l_ld, l_info, symbol intercept) nestačí.
> Runtime patch se provádí **v paměti po načtení parrot libc**, žádná změna na disku.

---

## Proč je potřeba

Parrot libc (glibc 2.41) inicializuje své `_rtld_global` v `.bss` při svém `.init`. Naše fake `ldso_global` je pro `_dl_*` symboly (přes symbol override), ale `GL(dl_ns)[0]._ns_loaded` čte parrot libc **přímo z vlastní `.bss`** (cached GOT entry nebo inline kód), ne přes náš override. Výsledek: `dl_iterate_phdr` vidí prázdný link_map list → crash.

---

## Co patchnout v načtené parrot libc

Po `load_module("libc.so.6")` a před `elf_run()`:

```c
static void patch_parrot_rtld_global(elf_object_t *libc_obj) {
    if (!libc_obj || !libc_obj->soname || !strstr(libc_obj->soname, "libc.so.6"))
        return;

    // 1. Najdi _rtld_global v parrot libc .bss
    //    - přes DT_DEBUG.r_map (pokud je nastaven) nebo symbol lookup v scope
    void *libc_rtld_global = NULL;
    {
        // Zkus najít přes DT_DEBUG v libci
        Elf64_Dyn *dyn = find_dynamic(libc_obj);
        for (Elf64_Dyn *d = dyn; d && d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_DEBUG) {
                struct r_debug *rdebug = (struct r_debug *)d->d_un.d_ptr;
                // rdebug->r_map = head of link_map list = _rtld_global._dl_loaded
                // _rtld_global je o offset_of(_dl_loaded) před r_map
                // v glibc 2.41: offsetof(_rtld_global, _dl_loaded) = 0x30 (approx)
                libc_rtld_global = (char *)rdebug->r_map - 0x30;
                break;
            }
        }
    }
    
    // Fallback: symbol lookup _rtld_global v našem scope (měl by vrátit náš ldso_global,
    // ale parrot libc má vlastní definici v .bss – musíme najít TU TU)
    if (!libc_rtld_global) {
        // Hledej v parrot libc .bss podle patternu: _dl_nns=0, _dl_loaded=0
        // nebo projdi .bss sekci libc_obj
    }
    
    if (!libc_rtld_global) return;

    // 2. Patchni _rtld_global pole (glibc 2.41 layout):
    //    typedef struct {
    //        struct r_debug _dl_debug;           // 0x00 - 0x30
    //        struct link_map *_dl_loaded;        // 0x30  ← GL(dl_ns)[0]._ns_loaded (starý název)
    //        struct link_map *_dl_lmwait;        // 0x38
    //        int _dl_nns;                        // 0x40
    //        struct dl_namespace _dl_ns[1];      // 0x48+
    //            struct dl_namespace {
    //                struct link_map *_ns_loaded; // 0x0
    //                unsigned int _ns_nloaded;    // 0x8
    //                ...
    //            };
    //        int dl_load_adds;                   // někde v .data
    //    } _rtld_global;

    // V glibc 2.41:
    // - _dl_ns array začíná cca na offsetu 0x48 (po _dl_nns)
    // - _dl_ns[0]._ns_loaded = offset 0x48
    // - _dl_nns = offset 0x40

    #define RTLD_GLOBAL_DL_NNS_OFF      0x40
    #define RTLD_GLOBAL_DL_NS_OFF       0x48
    #define DL_NS_NS_LOADED_OFF         0x00
    #define DL_NS_NS_NLOADED_OFF        0x08

    // Nastav počet namespaceů
    *(int *)((char *)libc_rtld_global + RTLD_GLOBAL_DL_NNS_OFF) = 1;
    
    // Nastav head link_map listu v namespace 0
    *(uintptr_t *)((char *)libc_rtld_global + RTLD_GLOBAL_DL_NS_OFF + DL_NS_NS_LOADED_OFF) 
        = (uintptr_t)ldso_exe_linkmap;
    
    // Nastav počet načtených modulů
    *(unsigned int *)((char *)libc_rtld_global + RTLD_GLOBAL_DL_NS_OFF + DL_NS_NS_NLOADED_OFF) 
        = 1 + ldso_module_count;

    // 3. Patchni dl_load_adds a dl_load_write_lock (offsety z ldso_setup)
    // v naší ldso_global jsou na 0xb18 a 0xab8 – v parrot libc mohou být jinde
    // Hledej podle patternu nebo použij známé offsety pro glibc 2.41

    if (elf_debug())
        fprintf(stderr, "[patch] parrot _rtld_global patched: _ns_loaded=%p\n", ldso_exe_linkmap);
}
```

---

## Ověření offsetů pro glibc 2.41

| Pole | Naše hardcoded offset | Poznámka |
|------|----------------------|----------|
| `_dl_nns` | 0xa80 (2688) | `g[0xa80/8] = 1` |
| `dl_load_adds` | 0xb18 (2840) | `g[0xb18/8] = 1` |
| `dl_load_write_lock` | 0xab8 | locked=0 |
| `_dl_ns[0]._ns_loaded` | **NEVÍM** | Klíčové pro `dl_iterate_phdr` |

**Akce:** Zkompilovat malý test program, který vypíše offsety z `sizeof(struct rtld_global)` a `offsetof` v glibc 2.41 headers, nebo rozbalit parrot `libc-dev` a podívat se do `link.h` / `dl-lookup.h`.

---

## TLS Kontrola (souvisí s btop/htop crashama)

### Současný stav TLS v loaderu
- `elf_setup_own_tls()` alokuje region, nastaví DTV, TCB head
- `new_tp` ukazuje na `tcbhead_t` s `dtv` pointerem
- `tp` switch dělá `elf_run_final()` těsně před entry (MSR TPIDR_EL0)

### Potenciální problémy u TUI/C++ aplikací
1. **`libstdc++` `std::string::reserve` crash (btop)**:
   - `libstdc++` používá `__gnu_cxx::__pool` pro malé stringy (SSO threshold)
   - Pool allokátor čte z TLS (`__thread` nebo `__builtin_thread_pointer()`)
   - Pokud `tp` neukazuje na validní parrot TLS → garbage pointery → crash v `reserve`

2. **`__stack_chk_guard` / `__pointer_chk_guard`**:
   - Loader poskytuje `ldso_stack_guard` a `ldso_pointer_chk_guard` přes `ldso_lookup`
   - Ale parrot libc může číst guardy **přímo z TLS** (offset pod TP), ne přes symbol
   - Offset v glibc 2.41: `TP - 0x28` pro `__stack_chk_guard` (?)
   - Naše TLS region je nulovaný → guardy = 0 → stack smashing detection nefunguje / false positives

3. **`malloc` thread_arena**:
   - Komentář v `elf_setup_own_tls`: `malloc thread_arena slot (TP-offset z libc .data @0x1afd68) zůstává NULL`
   - Parrot libc `malloc` (ptmalloc) čte arena pointer z TLS
   - NULL = "uninitialized" → glibc malloc si vezme `main_arena` (globální lock) → funguje, ale pomalejší
   - Pro TUI aplikace s více vlákny (htop, btop) může být problém

### Kontrola TLS offsetů v parrot libc
```bash
# V parrot rootfs:
readelf -s /usr/lib/aarch64-linux-gnu/libc.so.6 | grep -i "stack_chk_guard\|pointer_chk_guard\|rseq"
```

---

## Další kroky (před runtime patchem)

1. **Ověř offsety `_rtld_global` pro glibc 2.41** – stáhni glibc 2.41 source, podívej se do `elf/rtld.c` a `include/link.h`
2. **Přidej `l_tls_modid` (0x498) do fake link_map** – parrot libc ho čte pro TLS
3. **Ověř TLS guard offsety** – zkontroluj, zda parrot libc čte `__stack_chk_guard` z TLS nebo přes symbol
4. **Test bez `--ownall`** – spusť TUI s host bionic libc (`ashell -c "htop"`) pro izolaci problému
5. **Pouze pokud vše výše selže** → implementuj runtime patch

---

## Reference
- glibc 2.41 source: `elf/rtld.c` (struct rtld_global), `elf/dl-namespaces.c` (struct dl_namespace)
- Loader kód: `src/elf_loader.c` → `ldso_setup()`, `ldso_install_module_list()`, `elf_setup_own_tls()`