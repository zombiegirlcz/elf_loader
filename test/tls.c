#include <stdio.h>
#include <pthread.h>

static __thread int tls_var = 42;
static __thread char buf[64];

int main(void) {
    snprintf(buf, sizeof buf, "tls=%d", tls_var);
    puts(buf);
    tls_var++;
    printf("tls_var=%d buf=%s\n", tls_var, buf);
    return 0;
}
