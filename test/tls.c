#include <stdio.h>

static __thread int tls_var = 42;

int main(int argc, char **argv) {
    printf("TLS value: %d\n", tls_var);
    return 0;
}
