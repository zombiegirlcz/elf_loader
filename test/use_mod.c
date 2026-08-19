#include <stdio.h>

int mod_add(int, int);
int mod_count(void);

int main(void) {
    printf("mod_add(1,2)=%d\n", mod_add(1, 2));
    printf("mod_add(10,20)=%d\n", mod_add(10, 20));
    printf("count=%d\n", mod_count());
    return 0;
}