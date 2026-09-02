#include <stdio.h>

static __thread int tls_var = 123;

int get_tls_value(void) {
    return tls_var;
}

int main(int argc, char **argv) {
    printf("TLS-GD value: %d\n", tls_var);
    printf("TLS-GD getter: %d\n", get_tls_value());
    return 0;
}
