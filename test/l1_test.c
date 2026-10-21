/* l1_test.c — host test generatoru per-site stubu L1 (prompt 04).
 *
 * Nejrizikovejsi cast L1 jsou rucne zakodovane AArch64 instrukce stubu.
 * Tenhle test je proto NESPOKOJI s kontrolou bitu: stub skutecne vygeneruje,
 * patchne `svc #0` v testovacim kodu na skok do nej a cely blok SPUSTI.
 * Overuje:
 *   1) ze se skok do stubu i navrat na <misto+4> provedou,
 *   2) ze stub zachova x16/x17/x30 (per-site stub si je uklada sam, protoze
 *      `blr` do C je prepsat muze) a NZCV,
 *   3) ze syscall, ktery stub "obalil", pro volajiciho dal funguje
 *      (vysledek z l1_common-mocku dojde do x0),
 *   4) mezni hodnoty offsetu literalu a navratu (l1_emit_stub ma vratit 0).
 *
 * Bezi jen na aarch64. Build:
 *   cc -Wall -Iinclude -Isrc test/l1_test.c -o /tmp/l1t
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

#ifndef __aarch64__
int main(void) { printf("SKIP: jen aarch64\n"); return 0; }
#else

#define PAGE_SIZE 4096
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((uintptr_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uintptr_t)(a) - 1))

#include "l1_emit.h"   /* l1_emit_stub + L1_STUB_WORDS/L1_ISLAND_HDR */

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else printf("ok: %s\n", msg); \
} while (0)

/* Mock l1_common: bezna funkce (stub ji vola pres `blr x16`). Zapise do
 * pocitadla a vedome rozbije x16/x17 i navratovou hodnotu, aby bylo videt,
 * ze je stub obnovuje. */
static volatile int g_called = 0;
static volatile unsigned long g_frame_sp = 0;
__attribute__((noinline)) static void mock_common(void) {
    g_called++;
    /* uspatna hodnota do x16/x17 - stub je musi obnovit z vlastniho ramce */
    __asm__ volatile("mov x16, #0xdead\n\tmov x17, #0xbeef" ::: "x16", "x17");
    unsigned long sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    g_frame_sp = sp;
}

typedef long (*site_fn)(long a);

int main(void) {
    /* jeden mmap: [0] ostrov stubu, [4096] testovaci "modul" s `svc #0` */
    size_t sz = 2 * PAGE_SIZE;
    void *mem = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 2; }
    uint32_t *island = (uint32_t *)mem;
    uint32_t *code = (uint32_t *)((char *)mem + PAGE_SIZE);

    *(uintptr_t *)island = (uintptr_t)&mock_common;
    uintptr_t lit = (uintptr_t)island;
    uint32_t *stub = island + L1_ISLAND_HDR / 4;

    /* Testovaci funkce: vrati x0+1, ale uprostred ma `svc #0`, ktery
     * patchneme. Po navratu ze stubu musi pokracovat normalne a x16/x17/x30
     * musi byt tytez jako pred. Scheme:
     *   stp  x19, x20, [sp, #-16]!
     *   mov  x19, x16       // zapamatuj vstupni x16/x17
     *   mov  x20, x17
     *   svc  #0             // <- patch na `b stub`
     *   sub  x19, x19, x16  // 0, pokud se x16 obnovil
     *   sub  x20, x20, x17
     *   orr  x0, x19, x20   // 0 = oba obnoveny
     *   ldp  x19, x20, [sp], #16
     *   ret
     *
     * POZOR na x19/x20 misto x1/x2: mock_common je bezna C funkce, takze
     * caller-saved registry (x0-x18) prepsat SMI - zachovava je az skutecny
     * l1_common v entry.S. Prvni verze testu na tom FAILovala a vypadalo to
     * jako chyba stubu.
     */
    int n = 0;
    code[n++] = 0xa9bf53f3u;    /* stp x19, x20, [sp, #-16]! */
    code[n++] = 0xaa1003f3u;    /* mov x19, x16 */
    code[n++] = 0xaa1103f4u;    /* mov x20, x17 */
    size_t svc_idx = n;
    code[n++] = 0xd4000001u;    /* svc #0  -> bude patchnuto */
    code[n++] = 0xcb100273u;    /* sub x19, x19, x16 */
    code[n++] = 0xcb110294u;    /* sub x20, x20, x17 */
    code[n++] = 0xaa140260u;    /* orr x0, x19, x20 */
    code[n++] = 0xa8c153f3u;    /* ldp x19, x20, [sp], #16 */
    code[n++] = 0xd65f03c0u;    /* ret */

    uintptr_t site = (uintptr_t)&code[svc_idx];
    uintptr_t stub_addr = (uintptr_t)stub;
    int w = l1_emit_stub(stub, stub_addr, site + 4, lit);
    CHECK(w == L1_STUB_WORDS, "l1_emit_stub vygeneroval 9 instrukci");

    long off = (long)(stub_addr - site) / 4;
    code[svc_idx] = 0x14000000u | ((uint32_t)off & 0x3ffffffu);   /* b stub */
    __builtin___clear_cache((char *)mem, (char *)mem + sz);

    /* ── spust ─────────────────────────────────────────────────────────── */
    site_fn f = (site_fn)code;
    /* nastav x16/x17 na rozpoznatelne hodnoty pres inline asm wrapper */
    long r;
    __asm__ volatile(
        "mov x16, #0x1111\n\t"
        "mov x17, #0x2222\n\t"
        "mov x30, %2\n\t"          /* x30 na znamou hodnotu (stub ho uklada) */
        "blr %1\n\t"
        "mov %0, x0"
        : "=r"(r) : "r"(f), "r"(0x3333L) : "x0", "x16", "x17", "x30", "memory");

    CHECK(g_called == 1, "stub skocil do l1_common (prave jednou)");
    CHECK(r == 0, "stub obnovil x16 i x17 (jinak by x0 != 0)");
    CHECK(g_frame_sp != 0, "l1_common videl platny sp");

    /* druhe volani: zadna degradace (stub je bezstavovy) */
    g_called = 0;
    __asm__ volatile(
        "mov x16, #0x4444\n\t"
        "mov x17, #0x5555\n\t"
        "blr %1\n\t"
        "mov %0, x0"
        : "=r"(r) : "r"(f) : "x0", "x16", "x17", "x30", "memory");
    CHECK(g_called == 1 && r == 0, "druhe volani stejne (bezstavovy stub)");

    /* ── mezni offsety ─────────────────────────────────────────────────── */
    uint32_t tmp[L1_STUB_WORDS];
    CHECK(l1_emit_stub(tmp, 0x10000000u, 0x10000004u, 0x10000000u + (2u << 20)) == 0,
          "literal mimo +-1 MiB -> odmitnuto");
    CHECK(l1_emit_stub(tmp, 0x10000000u, 0x10000000u + (200u << 20), 0x10000000u) == 0,
          "navrat mimo +-128 MiB -> odmitnuto");
    CHECK(l1_emit_stub(tmp, 0x10000000u, 0x10000000u - (100u << 20), 0x10000000u) == L1_STUB_WORDS,
          "navrat -100 MiB -> prijato");

    /* ── kontrola, ze instrukce odpovidaji zamyslenemu kodovani ────────── */
    l1_emit_stub(tmp, stub_addr, site + 4, lit);
    CHECK(tmp[0] == 0xd10083ffu, "sub sp, sp, #32");
    CHECK(tmp[1] == 0xa90047f0u, "stp x16, x17, [sp]");
    CHECK(tmp[2] == 0xf9000bfeu, "str x30, [sp, #16]");
    CHECK((tmp[3] & 0xff00001fu) == 0x58000010u, "ldr x16, <literal>");
    CHECK(tmp[4] == 0xd63f0200u, "blr x16");
    CHECK(tmp[5] == 0xf9400bfeu, "ldr x30, [sp, #16]");
    CHECK(tmp[6] == 0xa94047f0u, "ldp x16, x17, [sp]");
    CHECK(tmp[7] == 0x910083ffu, "add sp, sp, #32");
    CHECK((tmp[8] & 0xfc000000u) == 0x14000000u, "b <misto+4>");

    munmap(mem, sz);
    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
#endif
