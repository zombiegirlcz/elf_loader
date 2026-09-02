#include <stdio.h>

// Test IFUNC (Indirect Function) resolution
__attribute__((ifunc("resolve_func"))) int my_add(int a, int b);

static int add_impl(int a, int b) {
    return a + b;
}

static void *resolve_func(void) {
    return (void *)add_impl;
}

int main(int argc, char **argv) {
    printf("IFUNC test: 2 + 3 = %d\n", my_add(2, 3));
    return 0;
}
