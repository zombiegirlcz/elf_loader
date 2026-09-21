/* once_test.cpp — rozhodujici reproducer pro node na parrot/elf_loader.
 *
 * Node (non-PIE ET_EXEC) ma jen 2 TLS_TPREL relokace:
 *   _ZSt11__once_call, _ZSt15__once_callable  (libstdc++ std::call_once)
 * Pokud nas tls_offset pro exe nesedi se zapečenym local-exec offsetem
 * v kodu, std::call_once zapisuje mimo TLS -> korupce glibc heapu.
 *
 * Testy:
 *   1) call_once jednou
 *   2) call_once ve smycce (stejna once_flag)
 *   3) malloc/free churn
 *   4) call_once z vice vlaken
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <mutex>
#include <thread>
#include <vector>

static std::once_flag g_flag;
static int g_counter = 0;

static void init_fn() {
    g_counter++;
    printf("init_fn ran, counter=%d\n", g_counter);
}

int main() {
    printf("== once_test start ==\n");
    fflush(stdout);

    printf("-- 1) call_once once --\n");
    std::call_once(g_flag, init_fn);
    printf("counter=%d\n", g_counter);
    fflush(stdout);

    printf("-- 2) call_once in loop (100x) --\n");
    for (int i = 0; i < 100; i++)
        std::call_once(g_flag, init_fn);
    printf("counter=%d (expect 1)\n", g_counter);
    fflush(stdout);

    printf("-- 3) malloc/free churn --\n");
    for (int i = 0; i < 5000; i++) {
        size_t sz = 16 + (size_t)(i % 2048);
        void *p = malloc(sz);
        if (!p) { printf("malloc FAIL i=%d\n", i); return 3; }
        memset(p, i & 0xff, sz);
        free(p);
    }
    printf("malloc churn ok\n");
    fflush(stdout);

    printf("-- 4) call_once from 4 threads --\n");
    static std::once_flag tflag;
    static int tcounter = 0;
    std::vector<std::thread> th;
    for (int i = 0; i < 4; i++) {
        th.emplace_back([]() {
            for (int k = 0; k < 50; k++) {
                std::call_once(tflag, []() { tcounter++; });
                void *p = malloc(64 + (k % 512));
                if (!p) { printf("thread malloc FAIL\n"); _exit(4); }
                free(p);
            }
        });
    }
    for (auto &t : th) t.join();
    printf("tcounter=%d (expect 1)\n", tcounter);

    printf("== once_test DONE ==\n");
    return 0;
}