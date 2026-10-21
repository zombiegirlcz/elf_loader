/* src/l1_emit.h — generator per-site stubu L1 (prompt 04).
 *
 * Samostatny soubor proto, aby sla ta nejrizikovejsi cast (rucne zakodovane
 * AArch64 instrukce) otestovat na hostu: test/l1_test.c stub skutecne
 * VYGENERUJE, PATCHNE do testovaciho kodu a SPUSTI. Vklada se do
 * src/elf_loader.c. */
#ifndef ELF_LOADER_L1_EMIT_H
#define ELF_LOADER_L1_EMIT_H

#define L1_STUB_WORDS 9
#define L1_ISLAND_HDR 16          /* literal s adresou l1_common (8 B) + rezerva */

/* Vygeneruje do `w` stub pro jedno misto; `ret_addr` = misto+4,
 * `lit` = adresa literalu s l1_common. Vraci pocet zapsanych instrukci. */
static int l1_emit_stub(uint32_t *w, uintptr_t stub_addr, uintptr_t ret_addr,
                        uintptr_t lit) {
    long lit_off = (long)(lit - (stub_addr + 3 * 4)) / 4;   /* ldr je 4. instrukce */
    long ret_off = (long)(ret_addr - (stub_addr + 8 * 4)) / 4;
    if (lit_off < -(1 << 18) || lit_off >= (1 << 18)) return 0;
    if (ret_off < -(1 << 25) || ret_off >= (1 << 25)) return 0;
    w[0] = 0xd10083ffu;                                  /* sub sp, sp, #32   */
    w[1] = 0xa90047f0u;                                  /* stp x16, x17, [sp] */
    w[2] = 0xf9000bfeu;                                  /* str x30, [sp, #16] */
    w[3] = 0x58000010u | ((uint32_t)(lit_off & 0x7ffff) << 5);  /* ldr x16, lit */
    w[4] = 0xd63f0200u;                                  /* blr x16           */
    w[5] = 0xf9400bfeu;                                  /* ldr x30, [sp, #16] */
    w[6] = 0xa94047f0u;                                  /* ldp x16, x17, [sp] */
    w[7] = 0x910083ffu;                                  /* add sp, sp, #32   */
    w[8] = 0x14000000u | ((uint32_t)ret_off & 0x3ffffffu); /* b <misto+4>     */
    return L1_STUB_WORDS;
}


#endif /* ELF_LOADER_L1_EMIT_H */
