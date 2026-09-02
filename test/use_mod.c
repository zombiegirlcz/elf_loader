#include <stdio.h>

extern int mod_add(int a, int b);

int main(int argc, char **argv) {
    printf("Result: %d\n", mod_add(1, 2));
    return 0;
}
