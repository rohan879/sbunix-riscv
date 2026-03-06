/*
 * test.c — SBUnix libc regression test
 *
 * Exercises every libc module and prints PASS/FAIL for each test.
 * All tested syscalls are now implemented in the kernel.
 *
 * Run from the shell:  test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* -----------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { \
        printf("  PASS  %s\n", label); \
        g_pass++; \
    } else { \
        printf("  FAIL  %s\n", label); \
        g_fail++; \
    } \
} while (0)

static void section(const char *name) {
    printf("\n[%s]\n", name);
}

/* -----------------------------------------------------------------------
 * 1. string.h
 * ---------------------------------------------------------------------- */

static void test_string(void) {
    section("string");

    CHECK("strlen hello",      strlen("hello") == 5);
    CHECK("strlen empty",      strlen("") == 0);

    char buf[16];
    memset(buf, 0xAB, sizeof(buf));
    int all_ab = 1;
    for (int i = 0; i < 16; i++)
        if ((unsigned char)buf[i] != 0xAB) { all_ab = 0; break; }
    CHECK("memset 0xAB", all_ab);

    char dst[8];
    memcpy(dst, "ABCDEFG", 7);
    dst[7] = '\0';
    CHECK("memcpy",            strcmp(dst, "ABCDEFG") == 0);

    CHECK("memcmp equal",      memcmp("abc", "abc", 3) == 0);
    CHECK("memcmp less",       memcmp("abc", "abd", 3) < 0);
    CHECK("memcmp greater",    memcmp("abd", "abc", 3) > 0);

    char s[64];
    strcpy(s, "hello");
    CHECK("strcpy",            strcmp(s, "hello") == 0);
    CHECK("strcmp eq",         strcmp("foo", "foo") == 0);
    CHECK("strcmp lt",         strcmp("abc", "abd") < 0);

    CHECK("strncmp prefix",    strncmp("foobar", "foobaz", 5) == 0);
    CHECK("strncmp differ",    strncmp("foobar", "foobaz", 6) < 0);

    char nc[8];
    strncpy(nc, "abcd", 3);
    nc[3] = '\0';
    CHECK("strncpy partial",   strncmp(nc, "abc", 3) == 0);

    strcpy(s, "foo");
    strcat(s, "bar");
    CHECK("strcat",            strcmp(s, "foobar") == 0);

    const char *p = strchr("abcabc", 'b');
    CHECK("strchr first",      p && *p == 'b' && p[1] == 'c');
    const char *q = strrchr("abcabc", 'b');
    CHECK("strrchr last",      q && q > p);

    const char *f = strstr("hello world", "world");
    CHECK("strstr found",      f && strcmp(f, "world") == 0);
    CHECK("strstr not found",  strstr("hello", "xyz") == (void *)0);
}

/* -----------------------------------------------------------------------
 * 2. stdlib.h — atoi, strtol, strtoul, itoa, abs, labs, rand, qsort,
 *               bsearch
 * ---------------------------------------------------------------------- */

static int int_cmp(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

static void test_stdlib(void) {
    section("stdlib");

    CHECK("atoi 42",           atoi("42")   ==  42);
    CHECK("atoi -7",           atoi("-7")   ==  -7);
    CHECK("atol 100",          atol("100")  == 100);

    char *end;
    CHECK("strtol hex",        strtol("0x1F", &end, 0)  == 0x1F);
    CHECK("strtol oct",        strtol("017",  &end, 0)  == 017);
    CHECK("strtol neg",        strtol("-99",  &end, 10) == -99);
    CHECK("strtoul 255",       strtoul("0xFF", &end, 0) == 255UL);
    CHECK("strtoul bin 8",     strtoul("1000", &end, 2) == 8UL);

    char ibuf[32];
    itoa(255, ibuf, 16);
    CHECK("itoa hex 255",      strcmp(ibuf, "ff") == 0);
    itoa(-1, ibuf, 10);
    CHECK("itoa -1",           strcmp(ibuf, "-1") == 0);
    itoa(0, ibuf, 10);
    CHECK("itoa 0",            strcmp(ibuf, "0") == 0);

    CHECK("abs pos",           abs(5)  ==  5);
    CHECK("abs neg",           abs(-5) ==  5);
    CHECK("labs neg",          labs(-1000000L) == 1000000L);

    srand(42);
    int r = rand();
    CHECK("rand range",        r >= 0 && r <= 32767);

    int arr[5] = {3, 1, 4, 1, 5};
    qsort(arr, 5, sizeof(int), int_cmp);
    CHECK("qsort sorted",      arr[0]==1 && arr[1]==1 && arr[2]==3 &&
                               arr[3]==4 && arr[4]==5);

    int key = 4;
    int *found = (int *)bsearch(&key, arr, 5, sizeof(int), int_cmp);
    CHECK("bsearch found",     found && *found == 4);
    int miss = 99;
    CHECK("bsearch not found", bsearch(&miss, arr, 5, sizeof(int), int_cmp) == (void *)0);
}

/* -----------------------------------------------------------------------
 * 3. stdio.h — sprintf, snprintf, dprintf
 * ---------------------------------------------------------------------- */

static void test_stdio(void) {
    section("stdio / printf variants");

    char buf[64];

    sprintf(buf, "%d + %d = %d", 1, 2, 3);
    CHECK("sprintf basic",     strcmp(buf, "1 + 2 = 3") == 0);

    sprintf(buf, "0x%x", 0xDEAD);
    CHECK("sprintf hex",       strcmp(buf, "0xdead") == 0);

    sprintf(buf, "%s", "hello");
    CHECK("sprintf str",       strcmp(buf, "hello") == 0);

    int n = snprintf(buf, 5, "hello");
    CHECK("snprintf truncate", strcmp(buf, "hell") == 0);
    CHECK("snprintf ret n",    n == 5);

    dprintf(2, "  (dprintf to stderr: ok)\n");
    CHECK("dprintf no crash",  1);
}

/* -----------------------------------------------------------------------
 * 4. unistd.h — getpid, getppid, pipe, dup2, open/read/close,
 *               chdir/getcwd, fork/waitpid, sleep
 * ---------------------------------------------------------------------- */

static void test_unistd(void) {
    section("unistd");

    /* getpid */
    pid_t pid = getpid();
    CHECK("getpid > 0",        pid > 0);

    /* getppid — should also be positive */
    pid_t ppid = getppid();
    CHECK("getppid > 0",       ppid > 0);

    /* pipe roundtrip */
    int fds[2];
    int r = pipe(fds);
    CHECK("pipe ok",           r == 0);
    if (r == 0) {
        write(fds[1], "X", 1);
        char c = 0;
        read(fds[0], &c, 1);
        CHECK("pipe roundtrip", c == 'X');
        close(fds[0]);
        close(fds[1]);
    }

    /* dup2 */
    int dup_fd = dup2(0, 9);
    CHECK("dup2 ok",           dup_fd == 9);
    if (dup_fd == 9)
        close(9);

    /* open + read a tarfs file */
    int fd = open("/bin/echo", O_RDONLY);
    CHECK("open /bin/echo",    fd >= 0);
    if (fd >= 0) {
        char rbuf[4];
        ssize_t nr = read(fd, rbuf, 4);
        /* ELF magic: 0x7f 'E' 'L' 'F' */
        CHECK("read ELF magic", nr == 4 && (unsigned char)rbuf[0] == 0x7f &&
              rbuf[1] == 'E' && rbuf[2] == 'L' && rbuf[3] == 'F');
        close(fd);
    }

    /* chdir / getcwd */
    char cwd_before[PATH_MAX];
    char cwd_after[PATH_MAX];
    getcwd(cwd_before, sizeof(cwd_before));
    r = chdir("/bin");
    CHECK("chdir /bin",        r == 0);
    getcwd(cwd_after, sizeof(cwd_after));
    CHECK("getcwd after chdir", strcmp(cwd_after, "/bin") == 0);
    chdir(cwd_before);

    /* fork / waitpid */
    pid_t child = fork();
    if (child == 0) {
        _exit(42);
    } else {
        CHECK("fork > 0",      child > 0);
        int status = 0;
        pid_t waited = waitpid(child, &status, 0);
        CHECK("waitpid child", waited == child);
        CHECK("WIFEXITED",     WIFEXITED(status));
        CHECK("WEXITSTATUS 42", WEXITSTATUS(status) == 42);
    }

    /* sleep(0) — should return immediately without hanging */
    sleep(0);
    CHECK("sleep(0) returns",  1);
}

/* -----------------------------------------------------------------------
 * 5. malloc — SYS_brk is implemented; malloc must succeed
 * ---------------------------------------------------------------------- */

static void test_malloc(void) {
    section("malloc / free / calloc / realloc");

    /* basic malloc */
    void *p = malloc(64);
    CHECK("malloc 64 != NULL", p != (void *)0);

    if (p) {
        /* write and read back */
        memset(p, 0xCD, 64);
        int ok = 1;
        for (int i = 0; i < 64; i++)
            if (((unsigned char *)p)[i] != 0xCD) { ok = 0; break; }
        CHECK("malloc writable", ok);
        free(p);
    }

    /* free(NULL) must not crash */
    free((void *)0);
    CHECK("free(NULL) no crash", 1);

    /* calloc — memory must be zeroed */
    int *arr = (int *)calloc(8, sizeof(int));
    CHECK("calloc != NULL",    arr != (void *)0);
    if (arr) {
        int zeroed = 1;
        for (int i = 0; i < 8; i++)
            if (arr[i] != 0) { zeroed = 0; break; }
        CHECK("calloc zeroed", zeroed);
        free(arr);
    }

    /* realloc — grow an allocation */
    char *s = (char *)malloc(8);
    CHECK("realloc src != NULL", s != (void *)0);
    if (s) {
        strcpy(s, "hello");
        char *s2 = (char *)realloc(s, 64);
        CHECK("realloc grow != NULL", s2 != (void *)0);
        if (s2) {
            CHECK("realloc preserved",  strcmp(s2, "hello") == 0);
            free(s2);
        }
    }

    /* multiple independent allocations */
    void *a = malloc(16), *b = malloc(16), *c = malloc(16);
    CHECK("multiple allocs",   a && b && c && a != b && b != c);
    free(a); free(b); free(c);
}

/* -----------------------------------------------------------------------
 * 6. time.h — nanosleep, clock_gettime
 * ---------------------------------------------------------------------- */

static void test_time(void) {
    section("time");

    /* nanosleep with zero delay */
    struct timespec ts = { 0, 0 };
    int r = nanosleep(&ts, (void *)0);
    CHECK("nanosleep({0,0})",  r == 0);

    /* clock_gettime — now implemented; must return 0 and fill tv_sec */
    struct timespec now;
    now.tv_sec = -1;
    now.tv_nsec = -1;
    r = clock_gettime(CLOCK_REALTIME, &now);
    CHECK("clock_gettime ret 0", r == 0);
    CHECK("clock_gettime tv_sec >= 0", now.tv_sec >= 0);
    CHECK("clock_gettime tv_nsec == 0", now.tv_nsec == 0);
}

/* -----------------------------------------------------------------------
 * 7. dirent.h — opendir / readdir / closedir on tarfs
 * ---------------------------------------------------------------------- */

static void test_dirent(void) {
    section("dirent");

    /* getdents64 is now implemented */
    DIR *d = opendir("/bin");
    CHECK("opendir /bin != NULL", d != (void *)0);

    if (d) {
        int count = 0;
        int found_echo = 0;
        struct dirent *de;
        while ((de = readdir(d)) != (void *)0) {
            count++;
            if (strcmp(de->d_name, "echo") == 0)
                found_echo = 1;
        }
        CHECK("readdir found entries", count > 0);
        CHECK("readdir found echo",    found_echo);
        closedir(d);
    }

    /* opendir on a non-existent path returns NULL */
    DIR *bad = opendir("/no/such/dir");
    CHECK("opendir bad path NULL", bad == (void *)0);
}

/* -----------------------------------------------------------------------
 * 8. sys/stat.h — fstat and stat
 * ---------------------------------------------------------------------- */

static void test_stat(void) {
    section("sys/stat");

    /* fstat on an open file */
    int fd = open("/bin/echo", O_RDONLY);
    CHECK("open for fstat",    fd >= 0);
    if (fd >= 0) {
        struct stat st;
        int r = fstat(fd, &st);
        CHECK("fstat ret 0",       r == 0);
        CHECK("fstat st_size > 0", st.st_size > 0);
        CHECK("fstat S_ISREG",     (st.st_mode & 0xF000) == 0x8000);
        close(fd);
    }

    /* stat by path */
    struct stat st2;
    int r2 = stat("/bin/echo", &st2);
    CHECK("stat ret 0",        r2 == 0);
    CHECK("stat st_size > 0",  st2.st_size > 0);
}

/* -----------------------------------------------------------------------
 * 9. lseek — repositioning within a tarfs file
 * ---------------------------------------------------------------------- */

static void test_lseek(void) {
    section("lseek");

    int fd = open("/bin/echo", O_RDONLY);
    CHECK("open for lseek",    fd >= 0);
    if (fd < 0) return;

    /* Read first byte */
    char b0;
    read(fd, &b0, 1);
    CHECK("first byte 0x7f",   (unsigned char)b0 == 0x7f);

    /* Seek back to start and re-read */
    long pos = lseek(fd, 0, SEEK_SET);
    CHECK("lseek SEEK_SET 0",  pos == 0);
    char b1;
    read(fd, &b1, 1);
    CHECK("re-read after seek", b1 == b0);

    /* SEEK_CUR: advance 2 bytes from current (now at offset 1) */
    long pos2 = lseek(fd, 2, SEEK_CUR);
    CHECK("lseek SEEK_CUR",    pos2 == 3);

    /* SEEK_END: seek to end */
    long sz = lseek(fd, 0, SEEK_END);
    CHECK("lseek SEEK_END > 0", sz > 0);

    close(fd);
}

/* -----------------------------------------------------------------------
 * 10. fcntl.h — constant values
 * ---------------------------------------------------------------------- */

static void test_fcntl(void) {
    section("fcntl constants");

    CHECK("O_RDONLY == 0",     O_RDONLY  == 0);
    CHECK("O_WRONLY == 1",     O_WRONLY  == 1);
    CHECK("O_RDWR   == 2",     O_RDWR    == 2);
    CHECK("O_CREAT defined",   O_CREAT   != 0);
    CHECK("O_TRUNC defined",   O_TRUNC   != 0);
    CHECK("AT_FDCWD == -100",  AT_FDCWD  == -100);
}

/* -----------------------------------------------------------------------
 * 11. errno.h — constant values and syscall error path
 * ---------------------------------------------------------------------- */

static void test_errno(void) {
    section("errno");

    CHECK("ENOENT == 2",       ENOENT == 2);
    CHECK("EBADF  == 9",       EBADF  == 9);
    CHECK("ENOMEM == 12",      ENOMEM == 12);
    CHECK("EACCES == 13",      EACCES == 13);
    CHECK("EINVAL == 22",      EINVAL == 22);
    CHECK("EPIPE  == 32",      EPIPE  == 32);

    int fd = open("/this/does/not/exist", O_RDONLY);
    CHECK("open nonexist -1",  fd == -1);
}

/* -----------------------------------------------------------------------
 * 12. limits.h
 * ---------------------------------------------------------------------- */

static void test_limits(void) {
    section("limits");

    CHECK("PATH_MAX == 4096",  PATH_MAX  == 4096);
    CHECK("NAME_MAX == 255",   NAME_MAX  == 255);
    CHECK("INT_MAX correct",   INT_MAX   == 2147483647);
    CHECK("PIPE_BUF > 0",      PIPE_BUF  > 0);
}

/* -----------------------------------------------------------------------
 * 13. signal.h — constant values; kill() with real pid
 * ---------------------------------------------------------------------- */

static void test_signal(void) {
    section("signal");

    CHECK("SIGINT  == 2",      SIGINT  == 2);
    CHECK("SIGKILL == 9",      SIGKILL == 9);
    CHECK("SIGTERM == 15",     SIGTERM == 15);
    CHECK("SIGCHLD == 17",     SIGCHLD == 17);
    CHECK("SIGPIPE == 13",     SIGPIPE == 13);
    CHECK("SIG_DFL == 0",      (long)SIG_DFL == 0L);

    /* kill with invalid pid returns -1 */
    int kr = kill(-999, 0);
    CHECK("kill bad pid -1",   kr == -1);
}

/* -----------------------------------------------------------------------
 * 14. sys/wait.h — WIFEXITED / WIFSIGNALED macro correctness
 * ---------------------------------------------------------------------- */

static void test_wait_macros(void) {
    section("sys/wait macros");

    int exited_0   = 0;
    int exited_1   = (1 << 8);
    int signalled9 = 9;

    CHECK("WIFEXITED(0)",          WIFEXITED(exited_0));
    CHECK("WEXITSTATUS(0)",        WEXITSTATUS(exited_0)  == 0);
    CHECK("WIFEXITED(1<<8)",       WIFEXITED(exited_1));
    CHECK("WEXITSTATUS(1<<8)",     WEXITSTATUS(exited_1)  == 1);
    CHECK("WIFSIGNALED(9)",        WIFSIGNALED(signalled9));
    CHECK("WTERMSIG(9)",           WTERMSIG(signalled9)   == 9);
}

/* -----------------------------------------------------------------------
 * 15. sys/mman.h — mmap/munmap now implemented
 * ---------------------------------------------------------------------- */

static void test_mman(void) {
    section("sys/mman");

    /* Anonymous mmap should return a valid (non-MAP_FAILED) pointer */
    void *p = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("mmap != MAP_FAILED",  p != MAP_FAILED);

    if (p != MAP_FAILED) {
        /* Memory must be readable and writable */
        volatile char *cp = (volatile char *)p;
        cp[0] = 0xAB;
        CHECK("mmap writable",   cp[0] == (char)0xAB);

        /* munmap should succeed */
        int r = munmap(p, 4096);
        CHECK("munmap == 0",     r == 0);
    }
}

/* -----------------------------------------------------------------------
 * 16. stdint.h — fixed-width type sizes
 * ---------------------------------------------------------------------- */

static void test_stdint(void) {
    section("stdint");

    CHECK("uint8_t  1 byte",   sizeof(uint8_t)  == 1);
    CHECK("uint16_t 2 bytes",  sizeof(uint16_t) == 2);
    CHECK("uint32_t 4 bytes",  sizeof(uint32_t) == 4);
    CHECK("uint64_t 8 bytes",  sizeof(uint64_t) == 8);
    CHECK("int8_t   1 byte",   sizeof(int8_t)   == 1);
    CHECK("int64_t  8 bytes",  sizeof(int64_t)  == 8);

    uint8_t  u8  = 0xFF;
    uint16_t u16 = 0xFFFF;
    uint32_t u32 = 0xFFFFFFFFU;
    int8_t   i8  = -128;
    CHECK("uint8_t  max",      u8  == 255U);
    CHECK("uint16_t max",      u16 == 65535U);
    CHECK("uint32_t max",      u32 == 0xFFFFFFFFU);
    CHECK("int8_t   min",      i8  == -128);
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== SBUnix libc test suite ===\n");

    test_string();
    test_stdlib();
    test_stdio();
    test_unistd();
    test_malloc();
    test_time();
    test_dirent();
    test_stat();
    test_lseek();
    test_fcntl();
    test_errno();
    test_limits();
    test_signal();
    test_wait_macros();
    test_mman();
    test_stdint();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
