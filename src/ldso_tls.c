/* ldso_tls.c — vlastní implementace _dl_allocate_tls / _dl_allocate_tls_init /
 * _dl_deallocate_tls pro elf_loader.
 *
 * Důvod: elf_loader zavádí guest ld.so (parrot glibc 2.41) jako běžnou .so,
 * ale nespouští jeho vlastní _dl_start — takže interní globální stav guest
 * ld.so (_rtld_global na base+0x40000, malloc cache na base+0x3fb00/0x3fb78)
 * zůstává nulový. Když libc přes lazy JUMP_SLOT zavolá _dl_allocate_tls,
 * resolve_jmp_symbol nejdřív zkusí override_lookup (má prioritu) a dostane
 * tuto naši funkci.
 *
 * Layout (aarch64 glibc, TLS_DTV_AT_TP — POZOR, NE TCB_AT_TP!):
 *   struct pthread     [TP - 0x720, TP)
 *   tcbhead_t          { dtv_t *dtv; void *private; }  na TP (16 B)
 *   TLS bloky          [TP + 0x10, TP + span)
 *   DTV                TP + roundup(span, 16)
 *
 * glibc konvence:
 *   GET_DTV(tp)       = ((tcbhead_t *)tp)->dtv   (ukazuje na &A[1])
 *   A[0].counter      = délka DTV (GET_DTV[-1])
 *   A[1].counter      = generace (GET_DTV[0])
 *   A[1 + modid].val  = TP + modul_off  (modid je 1-based)
 *
 * Offsety modulů (TP-relative) přiděluje elf_loader.c (elf_tls_assign) a
 * vystavuje přes elf_tls_module_count/at + elf_tls_span/static_size.
 */

#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../include/elf_loader.h"

#define TLS_PRE_TCB_SIZE 0x720u   /* sizeof(struct pthread) na aarch64 */
#define DTV_SURPLUS      16u
#define PAGE_ALIGN(x)    (((x) + 4095u) & ~(uintptr_t)4095u)

typedef struct { uintptr_t u[2]; } dtv2_t;   /* {counter | val,to_free} */

/* Zapíše DTV pro daný thread pointer `tp` a zkopíruje .tdata všech
 * registrovaných TLS modulů na tp + offset. Idempotentní. */
static void tls_setup_thread(uintptr_t tp) {
    size_t n = elf_tls_module_count();
    uintptr_t span = elf_tls_span();
    /* glibc cte THREAD_SELF->tid (TP-0x720+0xD0) v pthread_rwlock_rdlock
     * (porovnava __writer s tid) a v pthread_mutex (__owner). Kdyz je tid
     * nula a zamek odemceny (__writer/__owner == 0), glibc vraci falesny
     * EDEADLK (35) a OpenSSL zamky se neinicializuji. Napevno zapiseme
     * kernel tid pres raw svc (gettid=178) - inline asm, TP-independent. */
    {
        register long x8 __asm__("x8") = 178;
        register long x0 __asm__("x0") = 0;
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
        int *tid_slot = (int *)(tp - 0x650);   /* TP-0x720+0xD0 */
        *tid_slot = (x0 > 0) ? (int)x0 : 1;
    }
    /* rseq area je AZ za vsemi TLS bloky (TP+0x10 patri hlavnimu exe kvuli
     * zapečenym local-exec TPREL offsetum non-PIE binarek). */
    uintptr_t rseq_off = elf_tls_rseq_offset();
    memset((void *)(tp + rseq_off), 0xff, ELF_RSEQ_SIZE);
    dtv2_t *A = (dtv2_t *)(tp + rseq_off + ELF_RSEQ_SIZE);

    memset(A, 0, (2 + n + DTV_SURPLUS) * sizeof(dtv2_t));
    A[0].u[0] = (uintptr_t)(n + 1);   /* délka DTV */
    A[1].u[0] = 1;                    /* generace */

    for (size_t i = 0; i < n; i++) {
        elf_object_t *m = elf_tls_module_at(i);
        if (!m)
            continue;
        char *dst = (char *)tp + m->tls_offset;
        /* glibc: zkopiruj filesz z .tdata, zbytek do memsz vynuluj (.tbss). */
        if (m->tdata_src && m->tdata_filesz)
            memcpy(dst, m->tdata_src, m->tdata_filesz);
        if (m->tls_memsz > m->tdata_filesz)
            memset(dst + m->tdata_filesz, 0, m->tls_memsz - m->tdata_filesz);
        A[2 + i].u[0] = (uintptr_t)dst;   /* entry.val */
        A[2 + i].u[1] = 0;                /* to_free */
    }

    *(uintptr_t *)tp = (uintptr_t)&A[1];  /* tcbhead.dtv -> &A[1] */
}

void *ldso_allocate_tls(void *mem) {
    if (mem) {
        /* pthread_create předal předalokovaný TLS prostor (TP uvnitř
         * nového stacku; glibc rezervoval GLRO(dl_tls_static_size) nad ním). */
        tls_setup_thread((uintptr_t)mem);
        return mem;
    }
    /* mem == NULL: alokuj vlastní blok (struct pthread + TLS + DTV). */
    size_t n = elf_tls_module_count();
    uintptr_t span = elf_tls_span();
    uintptr_t rseq_off = elf_tls_rseq_offset();
    size_t need = TLS_PRE_TCB_SIZE
                  + rseq_off + ELF_RSEQ_SIZE
                  + (2 + n + DTV_SURPLUS) * sizeof(dtv2_t)
                  + 0x1000;
    size_t sz = PAGE_ALIGN(need);
    void *blk = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (blk == MAP_FAILED)
        return NULL;
    memset(blk, 0, sz);
    uintptr_t tp = (uintptr_t)blk + TLS_PRE_TCB_SIZE;
    tls_setup_thread(tp);
    return (void *)tp;
}

void *ldso_allocate_tls_init(void *result, int main_thread) {
    (void)main_thread;
    if (result)
        tls_setup_thread((uintptr_t)result);
    return result;
}

void ldso_deallocate_tls(void *tcb, int dealloc_tcb) {
    (void)tcb;
    (void)dealloc_tcb;
    /* DTV je uvnitř stack/TLS oblasti (uvolní se s ní); pro mem==NULL případ
     * necháváme blok leaknout — krátkověké procesy, bezpečnější než odhad
     * velikosti pro munmap. */
}
