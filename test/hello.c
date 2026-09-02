#include <stdio.h>

int main(int argc, char **argv) {
    printf("Hello from ELF loader!\n");
    for (int i = 1; i < argc; i++) {
        printf("Arg %d: %s\n", i, argv[i]);
    }
    return 0;
}
