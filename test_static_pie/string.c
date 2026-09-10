/* 05_string: string/memory functions */
#include <string.h>
#include <stdio.h>
int main(void) {
    char buf[32];
    memset(buf, 0, sizeof buf);
    strcpy(buf, "test");
    return (buf[0] == 't') ? 0 : 1;
}
