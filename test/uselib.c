#include <stdio.h>
extern int lib_get(void);
extern int lib_bump(void);
int main(void) {
    printf("lib_get=%d\n", lib_get());
    printf("after bump=%d\n", lib_bump());
    return 0;
}
