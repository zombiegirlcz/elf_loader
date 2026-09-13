/* 03_write: write() syscall directly */
#include <unistd.h>
int main(void) {
    const char *msg = "hello\n";
    write(1, msg, 6);
    return 0;
}
