/* l1_common_test.c — host test ASM tela L1 stubu (src/l1_common.S).
 *
 * Tohle je ta cast, kterou nelze "precist a doufat": l1_common musi obnovit
 * VSECHNY registry, protoze Linux syscall zachovava vse krome x0. Test proto
 * postavi kompletni cestu stub -> l1_common -> svc -> navrat a overi:
 *   1) l1_before dostane ramec s puvodnimi argumenty a spravne nr (z x8),
 *      a frame[22..29] (argumenty syscallu) je kopie frame[0..7],
 *   2) l1_before muze argument syscallu PREPSAT a jadro uz dostane novy
 *      (tak L1 preklada cesty) - ale guest po stubu dostane PUVODNI x1,
 *   3) l1_after vidi vysledek ve frame[20] a puvodni x0 ve frame[0]
 *      (kvuli getcwd, kde je x0 buffer),
 *   4) l1_after muze vysledek zmenit a volajici dostane ten zmeneny,
 *   5) x9/x10 (caller-saved), NZCV i d0 jsou po stubu nedotcene.
 * Negativni kontrola (rucne): rozbity l1_common (napr. bez obnovy x10 nebo
 * s obnovou x1 z prelozenych argumentu) musi tenhle test shodit.
 *
 * Build: cc -Wall -Iinclude -Isrc test/l1_common_test.c src/l1_common.S -o /tmp/l1ct
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef __aarch64__
int main(void) { printf("SKIP: jen aarch64\n"); return 0; }
#else

#define PAGE_SIZE 4096
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((uintptr_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uintptr_t)(a) - 1))
#include "l1_emit.h"

extern void l1_common(void);

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else printf("ok: %s\n", msg); \
} while (0)

/* ── mocky, ktere l1_common vola (v loaderu jsou v src/l1.h) ───────────── */
static int bf_calls, af_calls, bf_frame_bad, bf_copy_bad;
static long bf_nr, af_nr, af_ret, af_orig_x0;
static unsigned long bf_args[8];
static const char *g_replace_path;   /* != NULL -> l1_before prepise arg x1 */
static int g_skip_after;             /* 1 -> l1_before necha frame[21]=0 (rychla cesta) */
static long g_force_ret;             /* != 0    -> l1_after prepise vysledek */
static long g_deny;                  /* != 0    -> l1_before syscall odmitne */

long l1_before(unsigned long *frame, long nr) {
    bf_calls++;
    bf_nr = nr;
    for (int i = 0; i < 8; i++) bf_args[i] = frame[i];
    if (frame[21] != 0) bf_frame_bad++;
    for (int i = 0; i < 8; i++) if (frame[22 + i] != frame[i]) bf_copy_bad++;
    if (g_replace_path) frame[22 + 1] = (unsigned long)g_replace_path;
    /* kontrakt: frame[21] != 0 -> l1_common zavola l1_after (src/l1.h) */
    if (!g_skip_after) frame[21] = 1;
    /* Realny prekladac (C) smi caller-saved registry libovolne prepsat -
     * mock to dela umyslne, jinak by chybejici obnovu v l1_common nikdo
     * neodhalil (prvni negativni kontrola na to prosla). */
    __asm__ volatile("mov x9, #0xbad\n\tmov x10, #0xbad\n\tmov x11, #0xbad\n\t"
                     "mov x12, #0xbad\n\tmov x13, #0xbad\n\tmov x14, #0xbad\n\t"
                     "mov x15, #0xbad\n\tmov x16, #0xbad\n\tmov x17, #0xbad\n\t"
                     "movi d0, #0\n\tcmn xzr, xzr"
                     ::: "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                         "x16", "x17", "d0", "cc");
    /* odmitnuti (ro bind/mountpoint): vysledek do frame[20], bez l1_after */
    if (g_deny) { frame[20] = (unsigned long)g_deny; frame[21] = 0; return 1; }
    return 0;
}
void l1_after(unsigned long *frame, long nr) {
    af_calls++;
    af_nr = nr;
    af_ret = (long)frame[20];
    af_orig_x0 = (long)frame[0];
    if (g_force_ret) frame[20] = (unsigned long)g_force_ret;
    /* NZCV natvrdo na N=1 (test ocekava Z=1,C=1) - jinak by chybejici
     * `msr nzcv` v l1_common mohla projit, kdyby C kod nahodou nechal
     * stejne priznaky */
    __asm__ volatile("mov x9, #0x80000000\n\tmsr nzcv, x9" ::: "x9", "cc");
}

/* Testovaci "modul": openat(x0, x1, x2) s `svc #0`, ktery patchneme na skok
 * do stubu. Po svc porovna x9/x10 se znackou a x1 s puvodnim ukazatelem
 * (ulozenym v x19); pri neshode vrati x0 | (1<<16). */
typedef long (*site_fn)(long dfd, const char *path, long flags);

static void build_site(uint32_t *code, size_t *svc_idx) {
    int n = 0;
    code[n++] = 0xa9bf53f3u;    /*  0 stp x19, x20, [sp, #-16]! */
    code[n++] = 0xd2802269u;    /*  1 mov x9,  #0x113 */
    code[n++] = 0xd280226au;    /*  2 mov x10, #0x113 */
    code[n++] = 0xaa0103f3u;    /*  3 mov x19, x1   (puvodni ukazatel na cestu) */
    code[n++] = 0xd2800708u;    /*  4 mov x8,  #56  (openat) */
    *svc_idx = n;
    code[n++] = 0xd4000001u;    /*  5 svc #0   -> patch */
    code[n++] = 0xd2802274u;    /*  6 mov x20, #0x113 */
    code[n++] = 0xeb14013fu;    /*  7 cmp x9, x20 */
    code[n++] = 0x540000c1u;    /*  8 b.ne 14 */
    code[n++] = 0xeb14015fu;    /*  9 cmp x10, x20 */
    code[n++] = 0x54000081u;    /* 10 b.ne 14 */
    code[n++] = 0xeb13003fu;    /* 11 cmp x1, x19  (x1 musi byt PUVODNI) */
    code[n++] = 0x54000041u;    /* 12 b.ne 14 */
    code[n++] = 0x14000003u;    /* 13 b 16 (ok) */
    code[n++] = 0xd2a00029u;    /* 14 movz x9, #1, lsl #16 (znacka chyby) */
    code[n++] = 0xaa090000u;    /* 15 orr x0, x0, x9 */
    code[n++] = 0xa8c153f3u;    /* 16 ldp x19, x20, [sp], #16 */
    code[n++] = 0xd65f03c0u;    /* 17 ret */
}

int main(void) {
    size_t sz = 2 * PAGE_SIZE;
    uint32_t *mem = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 2; }
    uint32_t *island = mem;
    uint32_t *code = (uint32_t *)((char *)mem + PAGE_SIZE);
    *(uintptr_t *)island = (uintptr_t)&l1_common;
    uint32_t *stub = island + L1_ISLAND_HDR / 4;

    size_t svc_idx;
    build_site(code, &svc_idx);
    uintptr_t site = (uintptr_t)&code[svc_idx];
    if (!l1_emit_stub(stub, (uintptr_t)stub, site + 4, (uintptr_t)island)) {
        printf("FAIL: l1_emit_stub\n"); return 1;
    }
    long off = (long)((uintptr_t)stub - site) / 4;
    code[svc_idx] = 0x14000000u | ((uint32_t)off & 0x3ffffffu);
    __builtin___clear_cache((char *)mem, (char *)mem + sz);
    site_fn f = (site_fn)code;

    /* ── 1. prosty prubeh: openat existujiciho souboru ─────────────────── */
    long r = f(-100, "/etc/hostname", O_RDONLY);
    CHECK(bf_calls == 1 && af_calls == 1, "l1_before i l1_after zavolany prave jednou");
    CHECK(bf_frame_bad == 0, "l1_common vynuloval frame[21] (drzeny slot)");
    CHECK(bf_copy_bad == 0, "frame[22..29] (argumenty syscallu) = kopie frame[0..7]");
    CHECK(bf_nr == 56, "l1_before dostal nr=56 (openat) z x8");
    CHECK(bf_args[0] == (unsigned long)-100, "frame[0] = AT_FDCWD");
    CHECK(strcmp((const char *)bf_args[1], "/etc/hostname") == 0, "frame[1] = puvodni cesta");
    CHECK(r >= 0 && (r & (1L << 16)) == 0, "syscall probehl a x1, x9, x10 zachovany");
    CHECK(af_nr == 56 && af_ret == r, "l1_after vidi nr i vysledek ve frame[20]");
    CHECK(af_orig_x0 == -100, "l1_after vidi PUVODNI x0 ve frame[0] (kvuli getcwd)");
    if (r >= 0) close((int)(r & 0xffff));

    /* ── 2. l1_before prepise cestu -> syscall jde s novou, guest x1 puvodni */
    g_replace_path = "/etc/hosts";
    long r2 = f(-100, "/nonexistent-l1-test", O_RDONLY);
    CHECK(r2 >= 0 && (r2 & (1L << 16)) == 0,
          "prepsani argumentu ucinkovalo a guest dostal zpet PUVODNI x1");
    if (r2 >= 0) close((int)(r2 & 0xffff));
    g_replace_path = NULL;

    /* ── 3. l1_after prepise vysledek ──────────────────────────────────── */
    g_force_ret = -13;
    long r3 = f(-100, "/etc/hostname", O_RDONLY);
    CHECK(r3 == -13, "l1_after muze vysledek zmenit (fixup getcwd/readlinkat)");
    g_force_ret = 0;

    /* ── 4. chyba syscallu projde nezmenena ────────────────────────────── */
    long r4 = f(-100, "/nonexistent-l1-test", O_RDONLY);
    CHECK(r4 < 0 && r4 > -4096, "chybovy kod syscallu dojde k volajicimu");

    /* ── 4b. rychla cesta: frame[21] == 0 -> l1_after se NEvola ────────── */
    g_skip_after = 1;
    int af_before = af_calls;
    long r5 = f(-100, "/etc/hostname", O_RDONLY);
    CHECK(af_calls == af_before && r5 >= 0 && (r5 & (1L << 16)) == 0,
          "frame[21]==0 -> l1_after preskocen, syscall i registry v poradku");
    if (r5 >= 0) close((int)(r5 & 0xffff));
    g_skip_after = 0;

    /* ── 4c. odmitnuti: l1_before vrati != 0 -> svc se NEprovede, volajici
     * dostane frame[20], l1_after se nevola, registry zachovane. Pouzita cesta
     * existuje -> kdyby svc probehl, vratil by fd >= 0, ne -30. */
    g_deny = -30;
    af_before = af_calls;
    long r6 = f(-100, "/etc/hostname", O_RDONLY);
    CHECK(r6 == -30 && af_calls == af_before,
          "l1_before odmitl -> bez svc, vysledek -EROFS, l1_after preskocen");
    g_deny = 0;
    long r7 = f(-100, "/etc/hostname", O_RDONLY);
    CHECK(r7 >= 0 && (r7 & (1L << 16)) == 0, "po odmitnuti dalsi volani normalne (x1, x9, x10 ok)");
    if (r7 >= 0) close((int)(r7 & 0xffff));

    /* ── 5. NZCV a d0 ──────────────────────────────────────────────────── */
    unsigned long nz_before = 0, nz_after = 0;
    double q0_in = 1234.5, q0_out = 0;
    const char *path = "/etc/hostname";
    __asm__ volatile(
        "fmov d0, %d4\n\t"
        "cmp xzr, xzr\n\t"          /* Z=1, C=1 */
        "mrs %0, nzcv\n\t"
        "mov x0, #-100\n\t"
        "mov x1, %5\n\t"
        "mov x2, #0\n\t"
        "blr %6\n\t"
        "mrs %1, nzcv\n\t"
        "fmov %d2, d0\n\t"
        "mov %3, x0"
        : "=&r"(nz_before), "=&r"(nz_after), "=&w"(q0_out), "=&r"(r)
        : "w"(q0_in), "r"(path), "r"(f)
        : "x0", "x1", "x2", "x8", "x9", "x10", "x30", "d0", "memory");
    CHECK(q0_out == q0_in, "d0/q0 zachovano pres stub");
    if (r >= 0) close((int)(r & 0xffff));
    (void)nz_before; (void)nz_after;

    /* ── 6. NZCV: vlastni misto, ktere vrati priznaky HNED po svc ─────────
     * Prvni site je pouzit nelze: jeho `cmp` po svc priznaky prepise, takze
     * chybejici `msr nzcv` v l1_common by projela (negativni kontrola na to
     * opravdu prosla). */
    uint32_t *code2 = code + 64;
    code2[0] = 0xd2800708u;     /* mov x8, #56 */
    code2[1] = 0xd4000001u;     /* svc #0 -> patch */
    code2[2] = 0xd53b4200u;     /* mrs x0, nzcv */
    code2[3] = 0xd65f03c0u;     /* ret */
    uint32_t *stub2 = stub + L1_STUB_WORDS;
    uintptr_t site2 = (uintptr_t)&code2[1];
    l1_emit_stub(stub2, (uintptr_t)stub2, site2 + 4, (uintptr_t)island);
    long off2 = (long)((uintptr_t)stub2 - site2) / 4;
    code2[1] = 0x14000000u | ((uint32_t)off2 & 0x3ffffffu);
    __builtin___clear_cache((char *)mem, (char *)mem + sz);
    unsigned long nz_out = 0;
    __asm__ volatile(
        "mov x0, #-100\n\t"
        "mov x1, %1\n\t"
        "mov x2, #0\n\t"
        "cmp xzr, xzr\n\t"          /* Z=1, C=1 -> 0x60000000 */
        "blr %2\n\t"
        "mov %0, x0"
        : "=&r"(nz_out) : "r"(path), "r"(code2)
        : "x0", "x1", "x2", "x8", "x30", "cc", "memory");
    CHECK(nz_out == 0x60000000ul, "NZCV zachovano pres stub (Z=1,C=1)");

    munmap(mem, sz);
    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
#endif
