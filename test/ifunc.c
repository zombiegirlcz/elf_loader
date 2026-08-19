#include <stdio.h>
#include <stdlib.h>

static int fast_impl(int x) { return x * 2; }
static int slow_impl(int x) { return x * 100; }

static int (*select_impl(void))(int) {
    const char *m = getenv("IFUNC_MODE");
    return (m && m[0] == 's') ? slow_impl : fast_impl;
}

int resolve_fn(int) __attribute__((ifunc("select_impl")));

int main(void) {
    printf("ifunc(21) = %d\n", resolve_fn(21));
    return 0;
}
