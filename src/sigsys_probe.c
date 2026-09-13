/* Izolacni sonda: ktere syscally vraci app seccomp profil KILL?
 *
 * Motivation: starship pod loaderem umira na SIGSYS, ale nas SIGSYS handler
 * se vubec nespusti. To znamena, ze vyhrava SECCOMP_RET_KILL z aplikacniho
 * profilu (KILL < TRAP < ERRNO < ALLOW). Chceme najit presne cislo syscallu,
 * ktery starship zavola a app profil ho KILLuje.
 *
 * Postup: zmerime "akci" pro kazdy syscall nasim vlastnim zpusobem:
 *  - do docasneho procesu nainstalujeme seccomp filtr, ktery pro dany nr
 *    vraci SECCOMP_RET_LOG (jen loguje, nic neblokuje, nevyzaduje CAP).
 *    POZOR: SECCOMP_RET_LOG neni v kernelu 4.14? Je (od 4.14). Ale v
 *    stacked filtrech vyhrava nejnizsi akce, takze nas LOG nic neprebije.
 *  - proto misto LOG pouzijeme trik: filtr, ktery pro dany nr vraci
 *    SECCOMP_RET_ERRNO|0x42. Pokud se nas filtr uplatni (tzn. app profil
 *    pro dany nr vraci ALLOW nebo vyssí akci nez ERRNO), dostaneme EINVAL.
 *    Pokud app profil vraci KILL nebo TRAP, dostaneme SIGSYS/smrt.
 *  - rozliseni KILL vs TRAP: v rodici nainstalujeme SIGSYS handler; dite
 *    po navratu z syscallu vi: (a) errno==0x42 -> ALLOW/ERRNO, (b) SIGSYS
 *    handler zaznamenal -> TRAP, (c) dite zabito bez handleru -> KILL.
 *
 * Tato sonda se pousti v procesu, ktery UZ ma app profil (dedi ho), takze
 * merime presne to, co potrebujeme. Neni potreba fork - staci jeden
 * proces na syscall, ale fork je nutny, aby nas filtr neovlivnil zbytek.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SECCOMP_RET_LOG
#define SECCOMP_RET_LOG 0x7ffc0000U
#endif

static volatile long g_trapped_nr = -1;

static void probe_sigsys(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    g_trapped_nr = si->si_syscall;
}

/* Zjisti "akci" app profilu pro dany syscall cislo.
 * Vraci: 0=ALLOW/ERRNO, 1=TRAP, 2=KILL, -1=chyba */
static int probe_one(long nr)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* dite: nainstaluj handler, pak filtr ktery pro nr vraci ERRNO|0x42 */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = probe_sigsys;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSYS, &sa, NULL);

        struct sock_filter prog[4];
        prog[0] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                               offsetof(struct seccomp_data, nr));
        prog[1] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                               (unsigned)nr, 0, 1);
        prog[2] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                               SECCOMP_RET_ERRNO | 0x42);
        prog[3] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                               SECCOMP_RET_ALLOW);
        struct sock_fprog fp = { .len = 4, .filter = prog };
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fp);

        errno = 0;
        long r = syscall(nr, 0L, 0L, 0L, 0L, 0L, 0L);
        if (g_trapped_nr == nr)
            _exit(1);           /* TRAP */
        if (r == -1 && errno == 0x42)
            _exit(0);           /* ALLOW/ERRNO (nas filtr se uplatnil) */
        _exit(3);               /* neco jineho (EINVAL z jadra atd.) */
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS)
        return 2;               /* KILL */
    if (WIFEXITED(st)) {
        if (WEXITSTATUS(st) == 0) return 0;
        if (WEXITSTATUS(st) == 1) return 1;
    }
    return -1;
}

int main(int argc, char **argv)
{
    long lo = 0, hi = 500;
    if (argc > 1) lo = atol(argv[1]);
    if (argc > 2) hi = atol(argv[2]);
    for (long nr = lo; nr <= hi; nr++) {
        if (nr == SYS_exit || nr == SYS_exit_group)
            continue;
        int a = probe_one(nr);
        if (a == 2)
            fprintf(stderr, "[probe] nr=%ld KILL\n", nr);
        else if (a == 1)
            fprintf(stderr, "[probe] nr=%ld TRAP\n", nr);
    }
    fprintf(stderr, "[probe] done %ld..%ld\n", lo, hi);
    return 0;
}