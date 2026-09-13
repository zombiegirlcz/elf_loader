/* 07_fork: fork + _exit */
#include <unistd.h>
#include <sys/wait.h>
int main(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    if (pid < 0) return 1;
    int st = 0;
    wait(&st);
    return 0;
}
