/* src/l2.h — L2 (prompt 05): seccomp RET_TRAP zachytna sit pro guest kod,
 * ktery L1 neprepsal.
 *
 * L1 (prompt 04) prepisuje cestove `svc #0` v NACTENYCH modulech, ktere loader
 * zna (libc, ld.so). Kod, ktery svc vola sam a L1 ho nenaskenoval - staticke
 * binarky, Go (svc napric goroutinami), Bun (raw syscally), musl, JIT - zustava
 * nepokryt. L2 pro nej instaluje stackovany seccomp filtr: cestove syscally z
 * TEXTU GUEST EXE TRAPnou (SIGSYS) a handler cestu prelozi (stejny go_handle
 * jako Go mod). Text loaderu a pozdeji forknute HOST binarky lezi mimo rozsah
 * -> jich se filtr netkne (viz l2_install_range_filter v elf_loader.c).
 *
 * Vrstvy (kap. 2.3 studie): L1 = prepis pri nacteni (rychle, moduly), L2 =
 * zachytna sit (jeden signal na misto, pak s lazy prepisem ~0), L3 = jmenne
 * hooky (sémantika nad syscallem: own-load exec, setuid shimy).
 *
 * Vklada se do src/elf_loader.c ZA go_handle/elf_go_mode_setup, takze pouziva
 * l2_install_range_filter(), elf_text_range(), go_handle(), g_go_lo/g_go_hi,
 * okno (L2_WIN_*, g_l2_win_*) a pro lazy prepis l1_map_island/l1_emit_stub/
 * l1_common. Zapina ELF_LOADER_L2=1, staticke binarky ELF_LOADER_L2_STATIC=1,
 * lazy prepis ELF_LOADER_L2_LAZY=1 (default vyp, viz pozn. u l2_lazy_rewrite).
 */
#ifndef ELF_LOADER_L2_H
#define ELF_LOADER_L2_H

static int g_l2_enabled = -1;

int l2_enabled(void) {
    if (g_l2_enabled < 0) {
        const char *e = getenv("ELF_LOADER_L2");
        g_l2_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return g_l2_enabled;
}

/* main.c: pred nactenim hlavni exe (PIE pujde do okna), po nacteni nuluj
 * (zavislosti uz do okna nechceme - libc/ld.so resi L1). */
int  elf_l2_enabled(void) { return l2_enabled(); }
void elf_l2_prepare_exe_load(void) {
    if (!l2_enabled()) return;
    if (!l2_reserve_window())
        fprintf(stderr, "[l2] okno %lx se nepodarilo rezervovat (PIE exe pujde mimo)\n",
                (unsigned long)L2_WIN_LO);
    g_l2_want_window = 1;
}
void elf_l2_after_exe_load(void) { g_l2_want_window = 0; }

/* ── lazy prepis (opt-in ELF_LOADER_L2_LAZY=1) ───────────────────────────
 * Po prvnim zachyceni misto prepiseme na L1 stub (`b l1_common`), takze dalsi
 * volani uz signal nestoji (princip lazypoline). DEFAULT VYPNUTO: nahrada `svc`
 * -> `b` za behu NENI v mnozine instrukci, ktere ARM ARM (B2.3.4) dovoluje
 * menit SOUBEZNE s vykonavanim na jinem jadre (tam jen B/BL/BRK/NOP...), takze
 * u vicevlaknoveho guestu hrozi UNPREDICTABLE na soubezne bezicim jadre. L1 to
 * dela bezpecne pri nacteni (jeste pred vlakny). Pro jednovlaknove / jen cteci
 * zateze je lazy vyhra; meritelne az po A/B. */
static int g_l2_lazy = -1;
static uint32_t *g_l2_island = NULL;   /* sdileny ostrov stubu pro guest exe */
static uint32_t *g_l2_island_next = NULL;
static size_t    g_l2_island_cap = 0;  /* zbyva stubu */
static uintptr_t g_l2_island_lit = 0;

static int l2_lazy_on(void) {
    if (g_l2_lazy < 0) {
        const char *e = getenv("ELF_LOADER_L2_LAZY");
        g_l2_lazy = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return g_l2_lazy;
}

/* Prepise cestove `svc #0` na `site` (= pc-4) na skok do L1 stubu. Vola se z
 * SIGSYS handleru (async-signal kontext) -> jen raw mmap/mprotect, zadny malloc.
 * Bezpecne jen pokud guest tu instrukci nevykonava soubezne na jinem jadre. */
NOSP static void l2_lazy_rewrite(uintptr_t site) {
    if (!l2_lazy_on()) return;
    uint32_t *rt = (uint32_t *)site;
    if (*rt != 0xd4000001u) return;                  /* uz neni svc (prepsano) */
    if (!g_l2_island) {
        size_t want = 4096;                          /* ~450 stubu */
        g_l2_island = (uint32_t *)l1_map_island(site, want);
        if (!g_l2_island) { g_l2_lazy = 0; return; }
        *(uintptr_t *)g_l2_island = (uintptr_t)&l1_common;   /* literal */
        g_l2_island_lit = (uintptr_t)g_l2_island;
        g_l2_island_next = g_l2_island + L1_ISLAND_HDR / 4;
        g_l2_island_cap = (want - L1_ISLAND_HDR) / (L1_STUB_WORDS * 4);
    }
    if (g_l2_island_cap == 0) return;
    uintptr_t stub = (uintptr_t)g_l2_island_next;
    long off = (long)(stub - site) / 4;
    if (off < -(1 << 25) || off >= (1 << 25)) return;   /* mimo dosah `b` */
    if (!l1_emit_stub(g_l2_island_next, stub, site + 4, g_l2_island_lit)) return;
    __builtin___clear_cache((char *)g_l2_island_next,
                            (char *)(g_l2_island_next + L1_STUB_WORDS));
    mprotect(g_l2_island, 4096, PROT_READ | PROT_EXEC);
    uintptr_t pg = site & ~(uintptr_t)(PAGE_SIZE - 1);
    if (mprotect((void *)pg, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return;
    *rt = 0x14000000u | ((uint32_t)off & 0x3ffffffu);   /* b stub */
    __builtin___clear_cache((char *)rt, (char *)rt + 4);
    mprotect((void *)pg, PAGE_SIZE, PROT_READ | PROT_EXEC);
    g_l2_island_next += L1_STUB_WORDS;
    g_l2_island_cap--;
}

/* Instalace L2 filtru pro hlavni guest exe. Nastavuje g_go_lo/g_go_hi, takze
 * zachyceni obslouzi stavajici sigsys_handler (vetev `if (g_go_hi)` -> go_handle)
 * BEZE ZMENY. Volat z main.c po elf_patch_syscall_sites_all (misto Go modu -
 * L2 ho zahrnuje: jakakoli guest exe, nejen Go). 1 = aktivni. */
int elf_l2_mode_setup(elf_object_t *m) {
    if (!l2_enabled() || !g_f2_root || !m) return 0;
    unsigned long lo, hi;
    if (!elf_text_range(m, &lo, &hi)) return 0;
    int ok = l2_install_range_filter(lo, hi, "l2");
    if (ok && getenv("ELF_LOADER_DIAG"))
        fprintf(stderr, "[l2] exe text %lx-%lx v_okne=%d lazy=%d\n",
                lo, hi, (lo >= L2_WIN_LO && hi <= L2_WIN_HI), l2_lazy_on());
    return ok;
}

#endif /* ELF_LOADER_L2_H */
