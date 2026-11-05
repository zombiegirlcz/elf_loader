/* tools/sigsys-lock-probe.c — regresni sonda k pasti c. 35 (2026-11-05).
 *
 * Otazka: udrzi loader svuj SIGSYS handler, kdyz ho guest prepisuje RAW
 * rt_sigaction z adresy MIMO text guest exe? Presne to dela Bun 1.3 /
 * claude >= 2.1.317, ktery glibc `sigaction` uz neimportuje. Sonda jde
 * pres glibc `syscall(134,...)`, takze `svc` lezi v libc - tedy mimo
 * [g_go_lo,g_go_hi), kde konci IP check L2 filtru.
 *
 * Build (rootfs gcc, NE NDK):  gcc -O0 -o /tmp/sigsys-lock-probe tools/sigsys-lock-probe.c
 * Beh (host, viz past c. 5 - jen `ashell -c`):
 *   R=$ROOTFS; F2_SKIP=sigaction,signal ELF_LOADER_L2=1 ROOTFS=$R \
 *     $D/usr/bin/elf_loader --ownall $R/tmp/sigsys-lock-probe
 *   F2_SKIP vypne jmenny hook, aby se meril JEN filtr na hranici syscallu.
 *
 * Ocekavani: "OCHRANENO". "PREPSANO" = regrese obrany (nebo bezi
 * s ELF_LOADER_NO_SIGSYS_LOCK=1). Bez L2 je PREPSANO spravne - prefix
 * zije v l2_install_range_filter, tedy jen pod L2 / Go modem.
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

struct ksa { void *h; unsigned long flags; void *restorer; unsigned long mask; };
static void myh(int s, void *i, void *u) { (void)s; (void)i; (void)u; }

int main(void) {
    struct ksa old = {0}, act = {0};
    long r = syscall(134, 31L, 0L, (long)(unsigned long)&old, 8L);
    printf("[s2] pred:  rc=%ld handler=%p (loader)\n", r, old.h);
    void *loader_h = old.h;

    act.h = (void *)myh; act.flags = 4UL;
    r = syscall(134, 31L, (long)(unsigned long)&act, 0L, 8L);
    printf("[s2] zapis: rc=%ld\n", r);

    struct ksa chk = {0};
    syscall(134, 31L, 0L, (long)(unsigned long)&chk, 8L);
    printf("[s2] po:    handler=%p myh=%p\n", chk.h, (void *)myh);
    if (chk.h == (void *)myh)        printf("[s2] VERDIKT: PREPSANO (bug)\n");
    else if (chk.h == loader_h)      printf("[s2] VERDIKT: OCHRANENO (handler loaderu drzi)\n");
    else                             printf("[s2] VERDIKT: jiny handler %p\n", chk.h);
    return 0;
}
