typedef unsigned long ulong;
static long sc(long n, long a, long b, long c) {
    register long x0 __asm("x0") = a;
    register long x1 __asm("x1") = b;
    register long x2 __asm("x2") = c;
    register long x8 __asm("x8") = n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}
static void put(const char *s) {
    long n = 0;
    while (s[n]) n++;
    sc(64, 2, (long)s, n);
}
void go(long argc, char **argv) __attribute__((used, noinline));
void go(long argc, char **argv) {
    put("hello-nostdlib argc=");
    put(argv[0]);
    put("\n");
    sc(93, argc, 0, 0);
}