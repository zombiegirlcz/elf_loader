/* ft6.c — syscall probe: které syscalls app seccomp profil TRAPne?
 * Child per-probe (vfork-style clone), parent čte výsledek.
 *   exit kód childa: 200=syscall OK, errno=když -1, signál 31=TRAPnut
 */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <linux/sched.h>

struct probe { long nr; const char *name; unsigned long a1, a2, a3, a4; };

static struct probe P[] = {
    { 293, "rseq",           0, 0, 0, 0 },
    { 435, "clone3",         0, 0, 0, 0 },              /* NULL args */
    { 436, "close_range",    ~0UL - 1, ~0UL, 4, 0 },     /* invalid flags */
    { 437, "openat2",        (unsigned long)-100, 0, 0, 0 }, /* NULL how */
    { 439, "faccessat2",     (unsigned long)-100, 0, 4, 0 },
    { 441, "epoll_pwait2",   (unsigned long)-1, 0, 0, 0 },
    { 449, "futex_waitv",    0, 0, 0, 0 },
};

int main(void) {
    int nprobe = (int)(sizeof P / sizeof P[0]);
    printf("%-16s %-8s %s\n", "SYSCALL", "NR", "STAV");
    for (int i = 0; i < nprobe; i++) {
        fflush(stdout);
        pid_t pid = syscall(220, /* __NR_clone */
                            (unsigned long)(CLONE_VM | CLONE_VFORK | SIGCHLD),
                            0UL);
        if (pid == 0) {
            /* vfork child: jen syscall + _exit */
            long r = syscall(P[i].nr, P[i].a1, P[i].a2, P[i].a3, P[i].a4);
            _exit(r == -1 ? (errno == ENOSYS ? 38 : errno) : 200);
        }
        int st;
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st))
            printf("%-16s %-8ld TRAPPED (sig=%d) — blokováno sandboxesem\n",
                   P[i].name, P[i].nr, WTERMSIG(st));
        else {
            int code = WEXITSTATUS(st);
            if (code == 200)
                printf("%-16s %-8ld OK (volání proběhlo)\n", P[i].name, P[i].nr);
            else if (code == 38)
                printf("%-16s %-8ld ENOSYS (jádro nezná)\n", P[i].name, P[i].nr);
            else
                printf("%-16s %-8ld exists, errno=%d\n", P[i].name, P[i].nr, code);
        }
    }
    return 0;
}
