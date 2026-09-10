/* 04_malloc: heap allocation */
#include <stdlib.h>
int main(void) {
    char *p = malloc(64);
    if (!p) return 1;
    p[0] = 'A';
    free(p);
    return 0;
}
