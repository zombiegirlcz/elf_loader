/* 06_env: environment + getenv */
#include <stdlib.h>
int main(void) {
    const char *v = getenv("PATH");
    return (v != NULL) ? 0 : 1;
}
