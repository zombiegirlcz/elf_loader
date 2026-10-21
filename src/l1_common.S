/* ===== L1 (prompt 04): spolecne telo stubu pro patchnute cestove `svc #0`
 *
 * Per-site stub (generuje elf_l1_patch_module v src/elf_loader.c, lezi
 * v "ostrove" u modulu) vypada takto - x16/x17 i x30 si uklada sam, takze
 * tady je l1_common bezna funkce zakoncena `ret`:
 *
 *     sub  sp, sp, #32
 *     stp  x16, x17, [sp]        // IP0/IP1 muze C prepsat
 *     str  x30, [sp, #16]        // protoze volame `blr`
 *     ldr  x16, <literal>        // adresa l1_common
 *     blr  x16
 *     ldr  x30, [sp, #16]
 *     ldp  x16, x17, [sp]
 *     add  sp, sp, #32
 *     b    <site+4>              // staticky znamy navrat, zadny bl na site
 *
 * Linux syscall zachovava VSECHNY registry krome x0, takze guest kod se na
 * ne muze spolehat a my je musime obnovit do posledniho - vcetne NZCV a
 * celych q0-q31 (d8-d15 jsou callee-saved jen v dolni pulce, horni pulka
 * q8-q15 je caller-saved, takze ukladame vsech 32).
 *
 * Ramec L1_FRAME (752 B; x-pary maji u stp/ldp dosah jen 504 B, proto jsou
 * vsechny dole a q-pary (dosah 1008 B) nahore):
 *   0 x0,x1 | 16 x2,x3 | 32 x4,x5 | 48 x6,x7 | 64 x8,x9 | 80 x10,x11
 *   96 x12,x13 | 112 x14,x15 | 128 x18,nzcv | 144 x29,x30
 *   160 vysledek svc (frame[20]) | 168 drzeny slot (frame[21]; 0 = l1_after
 *       se nevola, 1 = jen fixup vysledku, jinak ukazatel na slot)
 *   176 argumenty SYSCALLU x0-x7 (frame[22..29]) - jen tyhle smi l1_before
 *       prepsat (prelozena cesta); frame[0..7] zustavaji PUVODNI
 *   240 q0-q31
 *
 * Proc dve kopie argumentu: syscall ABI zachovava vsechny registry krome x0,
 * takze guest musi po stubu videt v x1..x7 sve puvodni hodnoty - ne
 * ukazatel do naseho prekladoveho bufferu. Obnovujeme proto z frame[0..7],
 * do jadra jdou frame[22..29]. Vysledek se uklada mimo frame[0], aby
 * l1_after videl i puvodni x0 (getcwd(2) ma v x0 buffer ke stripnuti).
 *
 * l1_before/l1_after (C, src/l1.h) bezi pod GUEST TP -> jen raw syscally
 * a shim_str*, zadny bionic malloc/errno/snprintf. Signal guestu muze
 * dorazit uprostred, takze oba musi byt reentrantni (per-tid slot s urovni,
 * viz l1_slot_acquire). Samotny `svc #0` provadi tenhle kod v LOADERU, tedy
 * mimo pripadne guest okno L2 (prompt 05). */
#define L1_FRAME 752

.text
.balign 16
.globl l1_common
.type l1_common, %function
l1_common:
    sub sp, sp, #L1_FRAME
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    mrs x9, nzcv
    stp x18, x9, [sp, #128]
    stp x29, x30, [sp, #144]
    add x29, sp, #144
    stp q0, q1, [sp, #240]
    stp q2, q3, [sp, #272]
    stp q4, q5, [sp, #304]
    stp q6, q7, [sp, #336]
    stp q8, q9, [sp, #368]
    stp q10, q11, [sp, #400]
    stp q12, q13, [sp, #432]
    stp q14, q15, [sp, #464]
    stp q16, q17, [sp, #496]
    stp q18, q19, [sp, #528]
    stp q20, q21, [sp, #560]
    stp q22, q23, [sp, #592]
    stp q24, q25, [sp, #624]
    stp q26, q27, [sp, #656]
    stp q28, q29, [sp, #688]
    stp q30, q31, [sp, #720]
    stp x0, x1, [sp, #176]            /* x0-x7 jsou porad puvodni -> kopie */
    stp x2, x3, [sp, #192]            /* argumentu pro syscall (frame[22..]) */
    stp x4, x5, [sp, #208]
    stp x6, x7, [sp, #224]

    str xzr, [sp, #168]               /* frame[21]: zadny drzeny slot */
    mov x0, sp                        /* frame */
    mov x1, x8                        /* nr (i u dynamickeho x8 je to zdroj) */
    bl l1_before

    ldp x0, x1, [sp, #176]            /* argumenty SYSCALLU (mozna prelozene) */
    ldp x2, x3, [sp, #192]
    ldp x4, x5, [sp, #208]
    ldp x6, x7, [sp, #224]
    ldr x8, [sp, #64]
    svc #0
    str x0, [sp, #160]                /* frame[20]: vysledek (frame[0] = puvodni x0) */
    ldr x9, [sp, #168]                /* frame[21] == 0 -> l1_after neni treba */
    cbz x9, 1f                        /* (rychla cesta find/fts, viz l1_before) */
    mov x0, sp
    ldr x1, [sp, #64]
    bl l1_after
1:

    ldp q0, q1, [sp, #240]
    ldp q2, q3, [sp, #272]
    ldp q4, q5, [sp, #304]
    ldp q6, q7, [sp, #336]
    ldp q8, q9, [sp, #368]
    ldp q10, q11, [sp, #400]
    ldp q12, q13, [sp, #432]
    ldp q14, q15, [sp, #464]
    ldp q16, q17, [sp, #496]
    ldp q18, q19, [sp, #528]
    ldp q20, q21, [sp, #560]
    ldp q22, q23, [sp, #592]
    ldp q24, q25, [sp, #624]
    ldp q26, q27, [sp, #656]
    ldp q28, q29, [sp, #688]
    ldp q30, q31, [sp, #720]
    ldp x29, x30, [sp, #144]
    ldp x18, x9, [sp, #128]
    msr nzcv, x9
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]             /* x1..x7 PUVODNI (ne prelozene) */
    ldr x1, [sp, #8]
    ldr x0, [sp, #160]                /* vysledek, pripadne opraveny l1_after */
    add sp, sp, #L1_FRAME
    ret
.size l1_common, .-l1_common
