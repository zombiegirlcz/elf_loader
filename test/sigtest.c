/* sigtest.c — ověří, že elf_loader chainuje guest SIGSEGV handler.
 *
 * Guest (glibc) si zaregistruje vlastní SIGSEGV handler přes sigaction,
 * pak schválně zapíše do R--O stránky. Pokud loader guest handler zavolá,
 * uvidíme "GUEST-HANDLER". Pokud ne, dostaneme fault dump loaderu.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void handler(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    char buf[160];
    int n = snprintf(buf, sizeof buf, "GUEST-HANDLER sig=%d addr=%p\n", sig, si->si_addr);
    write(2, buf, n);
    void *page = (void *)((unsigned long)si->si_addr & ~4095UL);
    mprotect(page, 4096, PROT_READ | PROT_WRITE);
    _exit(42);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) { perror("sigaction"); return 1; }
    write(2, "installed-guest-sigsegv-handler\n", 32);
    void *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    write(2, "writing-to-ro-page...\n", 22);
    *(volatile char *)p = 42;
    write(2, "after-write (no fault?)\n", 24);
    return 0;
}