/* sa_layout.c — zjisti PRESNE offsety glibc struct sigaction na parrot
 * aarch64, aby slo spravne cist guest handler v elf_loaderovem override.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("sizeof(struct sigaction)=%zu\n", sizeof(struct sigaction));
    printf("offsetof(sa_handler)=%zu\n", offsetof(struct sigaction, sa_handler));
    printf("offsetof(sa_sigaction)=%zu\n", offsetof(struct sigaction, sa_sigaction));
    printf("offsetof(sa_mask)=%zu\n", offsetof(struct sigaction, sa_mask));
    printf("offsetof(sa_flags)=%zu\n", offsetof(struct sigaction, sa_flags));
    printf("offsetof(sa_restorer)=%zu\n", offsetof(struct sigaction, sa_restorer));
    printf("sizeof(sigset_t)=%zu\n", sizeof(sigset_t));

    /* demonstrace: napln handler a vypis raw slova */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = (void (*)(int, siginfo_t *, void *))0x1122334455667788UL;
    sa.sa_flags = 0x00000004; /* SA_SIGINFO */
    printf("raw:");
    unsigned long *w = (unsigned long *)&sa;
    for (size_t i = 0; i < sizeof sa / 8; i++)
        printf(" [%zu]=%016lx", i * 8, w[i]);
    printf("\n");
    return 0;
}