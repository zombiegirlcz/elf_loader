/* lxnative - spoustec glibc programu pres NATIVNI ld.so (prompt 12, prototyp).
 *
 * Co resi
 * -------
 * Nativni `ld.so --library-path … <prog>` v app domene umre na SIGSYS jeste
 * pred relokaci: glibc v `__tls_init_tp` vola `set_robust_list` (99) a `rseq`
 * (293) a app seccomp profil pro ne vraci SECCOMP_RET_TRAP. Jsou to jedine
 * dve prekazky - kdyz se SIGSYS pohlti, nativni ld.so dobehne glibc binarku
 * do konce (overeno na glibc 2.41 i 2.43).
 *
 * Pohltit je nelze zevnitr procesu:
 *   - vlastni seccomp filtr nepomuze, jadro bere z filtru NEJNIZSI akci
 *     a RET_TRAP (0x00030000) je nizsi nez RET_ERRNO (0x00050000);
 *   - SIGSYS handler by stacil, ale dispozice signalu se pri execve resetuji
 *     a `--audit` / `LD_PRELOAD` se zavola az PO `__tls_init_tp`
 *     (zmereno: `la_version` se bez pohlceni vubec nespusti);
 *   - SIG_IGN exec prezije, ale seccomp posila SIGSYS pres `force_sig_info`,
 *     ktery ignorovani prebije.
 *
 * Zbyva tracer. Narozdil od prootu to NENI preklad syscallu - zastavujeme se
 * jen na SIGSYS, tedy 2x za proces (a 2x za vlakno), ne u kazdeho syscallu.
 *
 * Proc freestanding a staticky: musi to byt binarka spustitelna v hostovem
 * (bionic) kontextu bez interpretu. Prelozi ji rootfs gcc, zadny NDK ani
 * Modal build:
 *
 *     gcc -O1 -static -nostdlib -nostartfiles -ffreestanding \
 *         -fno-stack-protector -o lxnative tools/lxnative.c
 *
 * Pouziti
 * -------
 *     lxnative [-v] [-a] <prog> [args…]
 *       -v  vypisovat pohlcene SIGSYS a ptrace udalosti
 *       -a  pohltit KAZDY SIGSYS (ladeni); default je jen allowlist nize,
 *           ostatni se propusti, aby skutecne problemy zustaly videt
 *
 * Omezeni prototypu: tracer neresi preklad cest. Guest vidi HOSTOVY korenovy
 * strom - na to je v promptu 12 audit knihovna (`liblx.so`).
 */

/* --- syscally (aarch64) --- */
#define NR_write        64
#define NR_exit_group   94
#define NR_clone        220
#define NR_execve       221
#define NR_wait4        260
#define NR_ptrace       117

/* --- ptrace --- */
#define PTRACE_TRACEME      0
#define PTRACE_CONT         7
#define PTRACE_SETOPTIONS   0x4200
#define PTRACE_GETSIGINFO   0x4202

#define PTRACE_O_TRACEFORK   0x00000002
#define PTRACE_O_TRACEVFORK  0x00000004
#define PTRACE_O_TRACECLONE  0x00000008
#define PTRACE_O_TRACEEXEC   0x00000010

#define SIGCHLD 17
#define SIGTRAP 5
#define SIGSTOP 19
#define SIGSYS  31
#define __WALL  0x40000000

#define MAX_PIDS 512

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory");
    return x0;
}

static void outs(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    sys6(NR_write, 2, (long)s, n, 0, 0, 0);
}

static void outn(long v)
{
    char b[24], o[24];
    int n = 0, i = 0;
    if (v < 0) { outs("-"); v = -v; }
    if (v == 0)
        b[n++] = '0';
    while (v > 0) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0)
        o[i++] = b[--n];
    sys6(NR_write, 2, (long)o, i, 0, 0, 0);
}

/* Syscally, ktere app profil TRAPuje a glibc je vola v ramci inicializace
 * vlakna. Jadro uz pred SIGSYS nastavilo x0 = -ENOSYS, takze pohlceni signalu
 * znamena "syscall vratil ENOSYS" - presne to, s cim glibc umi zit
 * (`__set_robust_list_avail` zustane false, rseq se nezaregistruje). */
static const int g_absorb[] = {
    99,   /* set_robust_list - main thread i kazde pthread_create */
    293,  /* rseq            - totez */
    435,  /* clone3          - glibc 2.34+ posix_spawn, fallback na clone()  */
    436,  /* close_range                                                     */
    437,  /* openat2                                                         */
    439,  /* faccessat2                                                      */
    /* uid/gid rodina: app profil ji TRAPuje taky a guest nastroje ji volaji
     * primo (zmereno: `busybox mktemp -d` umira na nr=144 setgid). Pod
     * own-loadem to odchyti uid shimy v libc, tady neni co zachytit, takze
     * ji pohlti tracer. ENOSYS je pro ne spravna odpoved jen nahodou -
     * spravne by tracer mel vracet 0 pres PTRACE_SETREGSET, jako to dela
     * fakeroot (prompt 08). Viz "Co zbyva" v docs/nextgen/proot-nahrada.md. */
    143,  /* setregid  */
    144,  /* setgid    */
    145,  /* setreuid  */
    146,  /* setuid    */
    149,  /* getresgid */
};

static int absorbed(int nr)
{
    for (unsigned i = 0; i < sizeof g_absorb / sizeof g_absorb[0]; i++)
        if (g_absorb[i] == nr)
            return 1;
    return 0;
}

/* Prvni zastaveni noveho tracee je SIGSTOP, ktery mu musime spolknout.
 * Dalsi SIGSTOP uz je skutecny a propousti se. */
/* Inicializovane (= nenulove v souboru), aby RW segment nemel FileSiz 0.
 * Cisty .bss vyrobi PT_LOAD s p_filesz == 0 a jadro na tomhle zarizeni
 * takovy segment nenamapuje -> SIGSEGV jeste pred prvni instrukci programu.
 * Viz past v AGENTS.md. */
static long g_seen[MAX_PIDS] = { -1 };
static int g_nseen = 0;

static int first_stop(long pid)
{
    for (int i = 0; i < g_nseen; i++)
        if (g_seen[i] == pid)
            return 0;
    if (g_nseen < MAX_PIDS)
        g_seen[g_nseen++] = pid;
    return 1;
}

__asm__(".global _start\n_start:\n\tmov x0, sp\n\tb lxn_start\n");

void lxn_start(long *sp)
{
    long argc = sp[0];
    char **argv = (char **)&sp[1];
    char **envp = &argv[argc + 1];
    int verbose = 0, absorb_all = 0;

    while (argc >= 2 && argv[1][0] == '-' && argv[1][1] != 0 && argv[1][2] == 0) {
        if (argv[1][1] == 'v')      verbose = 1;
        else if (argv[1][1] == 'a') absorb_all = 1;
        else break;
        argc--; argv++;
    }
    if (argc < 2) {
        outs("pouziti: lxnative [-v] [-a] <prog> [args…]\n");
        sys6(NR_exit_group, 2, 0, 0, 0, 0, 0);
    }

    long root = sys6(NR_clone, SIGCHLD, 0, 0, 0, 0, 0);
    if (root == 0) {
        sys6(NR_ptrace, PTRACE_TRACEME, 0, 0, 0, 0, 0);
        sys6(NR_execve, (long)argv[1], (long)&argv[1], (long)envp, 0, 0, 0);
        outs("lxnative: execve selhal: ");
        outs(argv[1]);
        outs("\n");
        sys6(NR_exit_group, 127, 0, 0, 0, 0, 0);
    }
    if (root < 0) {
        outs("lxnative: fork selhal\n");
        sys6(NR_exit_group, 127, 0, 0, 0, 0, 0);
    }

    long rc = 0;
    int opts_set = 0;
    unsigned char si[128];

    for (;;) {
        int st = 0;
        long pid = sys6(NR_wait4, -1, (long)&st, __WALL, 0, 0, 0);
        if (pid < 0)
            break;                                  /* ECHILD - vse dobehlo */

        if ((st & 0xff) == 0x7f) {                  /* WIFSTOPPED */
            int sig = (st >> 8) & 0xff;
            int event = (st >> 16) & 0xff;

            if (!opts_set && pid == root) {
                /* Potomci i vlakna se musi pripojit automaticky - jinak jim
                 * jejich vlastni set_robust_list nikdo nepohlti. */
                sys6(NR_ptrace, PTRACE_SETOPTIONS, pid, 0,
                     PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                     PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC, 0, 0);
                opts_set = 1;
            }

            if (sig == SIGSYS) {
                int nr = -1;
                if (sys6(NR_ptrace, PTRACE_GETSIGINFO, pid, 0, (long)si, 0, 0) == 0)
                    nr = *(int *)(si + 24);         /* siginfo_t._sigsys._syscall */
                int take = absorb_all || (nr >= 0 && absorbed(nr));
                if (verbose) {
                    outs(take ? "[lxnative] pohlcen SIGSYS nr=" :
                                "[lxnative] PROPUSTEN SIGSYS nr=");
                    outn(nr);
                    outs(" pid=");
                    outn(pid);
                    outs("\n");
                }
                sys6(NR_ptrace, PTRACE_CONT, pid, 0, take ? 0 : SIGSYS, 0, 0);
                continue;
            }
            if (sig == SIGTRAP || (event != 0 && sig == SIGTRAP)) {
                sys6(NR_ptrace, PTRACE_CONT, pid, 0, 0, 0, 0);
                continue;
            }
            if (sig == SIGSTOP && first_stop(pid)) {
                sys6(NR_ptrace, PTRACE_CONT, pid, 0, 0, 0, 0);
                continue;
            }
            sys6(NR_ptrace, PTRACE_CONT, pid, 0, sig, 0, 0);
            continue;
        }

        if (pid == root) {
            if ((st & 0x7f) != 0)
                rc = 128 + (st & 0x7f);             /* WIFSIGNALED */
            else
                rc = (st >> 8) & 0xff;              /* WIFEXITED   */
        }
    }
    sys6(NR_exit_group, rc, 0, 0, 0, 0, 0);
}
