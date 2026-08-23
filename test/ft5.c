/* raw clone fork-style test — bionic */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
int main(void) {
    static int tid_slot;
    printf("pre-clone\n"); fflush(stdout);
    long r = syscall(220 /*clone*/,
                     (unsigned long)(0x00020000 | 0x01000000 | 17 /*SIGCHLD*/),
                     0UL, 0UL, 0UL, (unsigned long)&tid_slot);
    if (r == 0) {
        write(1, "child-ok\n", 9);
        _exit(44);
    }
    printf("parent: r=%ld errno=%d\n", r, errno);
    if (r > 0) {
        int st; waitpid((pid_t)r, &st, 0);
        printf("child sig=%d exit=%d\n",
               WIFSIGNALED(st) ? WTERMSIG(st) : 0,
               WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }
    return 0;
}
