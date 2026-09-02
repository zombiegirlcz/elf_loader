#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv, char **envp) {
    printf("Environment test:\n");
    printf("argc: %d\n", argc);
    for (int i = 0; i < argc && argv[i]; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }
    printf("Environment variables:\n");
    for (int i = 0; envp && envp[i]; i++) {
        printf("  %s\n", envp[i]);
    }
    return 0;
}
