/*
 * thirdpartytest.c -- integration coverage for vendored external workloads.
 *
 * These checks run the staged Micropython, BusyBox, pdpmake, and TCC payloads
 * against the real SBUnix filesystem/process stack.  The goal is to catch
 * regressions that native test binaries can miss in external runtimes.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
  if (cond) { \
    printf("[PASS] thirdparty: %s\n", label); \
    g_pass++; \
  } else { \
    printf("[FAIL] thirdparty: %s\n", label); \
    g_fail++; \
  } \
} while (0)

static int read_all(int fd, char *buf, int bufsz)
{
  int total = 0;

  while (total < bufsz - 1) {
    ssize_t n = read(fd, buf + total, bufsz - 1 - total);
    if (n <= 0)
      break;
    total += (int)n;
  }
  buf[total] = '\0';
  return total;
}

static int run_capture(const char *path, char *const argv[],
                       char *buf, int bufsz, int *status)
{
  int pfd[2];
  pid_t pid;

  if (pipe(pfd) != 0)
    return -1;

  pid = fork();
  if (pid < 0) {
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }

  if (pid == 0) {
    close(pfd[0]);
    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);
    execve(path, argv, environ);
    _exit(127);
  }

  close(pfd[1]);
  read_all(pfd[0], buf, bufsz);
  close(pfd[0]);
  waitpid(pid, status, 0);
  return 0;
}

static int write_text(const char *path, const char *text, int mode)
{
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  size_t len = strlen(text);
  ssize_t nw;

  if (fd < 0)
    return -1;
  nw = write(fd, text, len);
  close(fd);
  return nw == (ssize_t)len ? 0 : -1;
}

static int read_file(const char *path, char *buf, int bufsz)
{
  int fd = open(path, O_RDONLY);
  int nr;

  if (fd < 0)
    return -1;
  nr = read_all(fd, buf, bufsz);
  close(fd);
  return nr;
}

static int ensure_dir(const char *path)
{
  if (mkdir(path, 0755) == 0)
    return 0;
  return errno == EEXIST ? 0 : -1;
}

static void test_micropython(void)
{
  const char *base = "/mnt/thirdpartytest-mpy";
  const char *alpha = "/mnt/thirdpartytest-mpy/alpha.txt";
  const char *beta = "/mnt/thirdpartytest-mpy/beta.txt";
  const char *empty = "/mnt/thirdpartytest-mpy/empty";
  const char *script_path = "/mnt/thirdpartytest-mpy.py";
  const char *script =
      "import os\n"
      "base='/mnt/thirdpartytest-mpy'\n"
      "print('cwd=' + os.getcwd())\n"
      "print('hasalpha=' + str('alpha.txt' in os.listdir(base)))\n"
      "print('size=' + str(os.stat(base + '/alpha.txt')[6]))\n"
      "os.rename(base + '/alpha.txt', base + '/beta.txt')\n"
      "print('hasbeta=' + str('beta.txt' in os.listdir(base)))\n"
      "os.chdir(base)\n"
      "print('cwd2=' + os.getcwd())\n"
      "os.remove('beta.txt')\n"
      "os.rmdir('empty')\n"
      "print('done')\n";
  char *argv[] = {"/bin/micropython", (char *)script_path, NULL};
  char buf[1024];
  int status = 0;

  unlink(alpha);
  unlink(beta);
  unlink(script_path);
  rmdir(empty);
  rmdir(base);

  CHECK("micropython: base dir created", ensure_dir(base) == 0);
  CHECK("micropython: fixture file created",
        write_text(alpha, "ALPHA", 0644) == 0);
  CHECK("micropython: empty dir created", mkdir(empty, 0755) == 0);
  CHECK("micropython: script written", write_text(script_path, script, 0644) == 0);

  CHECK("micropython: exec succeeds",
        run_capture("/bin/micropython", argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK("micropython: cwd starts at /", strstr(buf, "cwd=/\n") != NULL);
  CHECK("micropython: listdir sees fixture",
        strstr(buf, "hasalpha=True\n") != NULL);
  CHECK("micropython: stat reports file size",
        strstr(buf, "size=5\n") != NULL);
  CHECK("micropython: rename observed in guest",
        strstr(buf, "hasbeta=True\n") != NULL);
  CHECK("micropython: chdir updates cwd",
        strstr(buf, "cwd2=/mnt/thirdpartytest-mpy\n") != NULL);
  CHECK("micropython: script completed",
        strstr(buf, "done\n") != NULL);
  CHECK("micropython: alpha removed", access(alpha, F_OK) != 0);
  CHECK("micropython: beta removed", access(beta, F_OK) != 0);
  CHECK("micropython: empty dir removed", access(empty, F_OK) != 0);

  unlink(script_path);
  rmdir(base);
}

static void test_busybox(void)
{
  const char *base = "/mnt/thirdpartytest-busybox";
  const char *src = "/mnt/thirdpartytest-busybox/src.txt";
  const char *copy = "/mnt/thirdpartytest-busybox/copy.txt";
  const char *moved = "/mnt/thirdpartytest-busybox/moved.txt";
  const char *subdir = "/mnt/thirdpartytest-busybox/subdir";
  char *cp_argv[] = {"busybox", "cp", (char *)src, (char *)copy, NULL};
  char *mv_argv[] = {"mv", (char *)copy, (char *)moved, NULL};
  char *mkdir_argv[] = {"mkdir", (char *)subdir, NULL};
  char *ls_argv[] = {"ls", (char *)base, NULL};
  char *cat_argv[] = {"busybox", "cat", (char *)moved, NULL};
  char *rm_argv[] = {"rm", (char *)src, (char *)moved, NULL};
  char *rmdir_subdir_argv[] = {"rmdir", (char *)subdir, NULL};
  char *rmdir_base_argv[] = {"rmdir", (char *)base, NULL};
  char buf[1024];
  int status = 0;

  unlink(src);
  unlink(copy);
  unlink(moved);
  rmdir(subdir);
  rmdir(base);

  CHECK("busybox: base dir created", ensure_dir(base) == 0);
  CHECK("busybox: source file created",
        write_text(src, "busybox-data\n", 0644) == 0);

  CHECK("busybox: multicall cp works",
        run_capture("/bin/busybox", cp_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK("busybox: symlink mv works",
        run_capture("/usr/lib/busybox/bin/mv", mv_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK("busybox: symlink mkdir works",
        run_capture("/usr/lib/busybox/bin/mkdir", mkdir_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK("busybox: multicall cat returns copied data",
        run_capture("/bin/busybox", cat_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        strcmp(buf, "busybox-data\n") == 0);
  CHECK("busybox: ls sees moved file and subdir",
        run_capture("/usr/lib/busybox/bin/ls", ls_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        strstr(buf, "moved.txt") != NULL &&
        strstr(buf, "subdir") != NULL);
  CHECK("busybox: symlink rm removes files",
        run_capture("/usr/lib/busybox/bin/rm", rm_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        access(src, F_OK) != 0 &&
        access(moved, F_OK) != 0);
  CHECK("busybox: symlink rmdir removes subdir",
        run_capture("/usr/lib/busybox/bin/rmdir", rmdir_subdir_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        access(subdir, F_OK) != 0);
  CHECK("busybox: symlink rmdir removes base dir",
        run_capture("/usr/lib/busybox/bin/rmdir", rmdir_base_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        access(base, F_OK) != 0);
}

static void write_makefile(char *buf, int bufsz, const char *base)
{
  snprintf(buf, bufsz,
           "all: final\n"
           "final: one two\n"
           "\tcat %s/one.txt %s/two.txt > %s/final.txt\n"
           "\t/bin/echo made > %s/stamp.txt\n"
           "one:\n"
           "\t/bin/echo ONE > %s/one.txt\n"
           "two:\n"
           "\t/bin/echo TWO > %s/two.txt\n",
           base, base, base, base, base, base);
}

static void cleanup_make_outputs(const char *base)
{
  char path[256];

  snprintf(path, sizeof(path), "%s/one.txt", base);
  unlink(path);
  snprintf(path, sizeof(path), "%s/two.txt", base);
  unlink(path);
  snprintf(path, sizeof(path), "%s/final.txt", base);
  unlink(path);
  snprintf(path, sizeof(path), "%s/stamp.txt", base);
  unlink(path);
}

static void verify_make_outputs(const char *base, const char *label_prefix)
{
  char final_path[256];
  char stamp_path[256];
  char buf[128];
  char label[96];

  snprintf(final_path, sizeof(final_path), "%s/final.txt", base);
  snprintf(stamp_path, sizeof(stamp_path), "%s/stamp.txt", base);

  snprintf(label, sizeof(label), "%s final output created", label_prefix);
  CHECK(label, access(final_path, F_OK) == 0);
  if (read_file(final_path, buf, sizeof(buf)) >= 0) {
    snprintf(label, sizeof(label), "%s merged output content", label_prefix);
    CHECK(label, strcmp(buf, "ONE\nTWO\n") == 0);
  } else {
    snprintf(label, sizeof(label), "%s merged output content", label_prefix);
    CHECK(label, 0);
  }

  if (read_file(stamp_path, buf, sizeof(buf)) >= 0) {
    snprintf(label, sizeof(label), "%s stamp content", label_prefix);
    CHECK(label, strcmp(buf, "made\n") == 0);
  } else {
    snprintf(label, sizeof(label), "%s stamp content", label_prefix);
    CHECK(label, 0);
  }
}

static void test_pdpmake(void)
{
  const char *base = "/mnt/thirdpartytest-make";
  const char *makefile = "/mnt/thirdpartytest-make/Makefile";
  char makebuf[1024];
  char outbuf[1024];
  char *pd_argv[] = {"pdpmake", "-f", (char *)makefile, "all", NULL};
  char *make_argv[] = {"make", "-f", (char *)makefile, "all", NULL};
  int status = 0;

  cleanup_make_outputs(base);
  unlink(makefile);
  rmdir(base);

  CHECK("pdpmake: base dir created", ensure_dir(base) == 0);
  write_makefile(makebuf, sizeof(makebuf), base);
  CHECK("pdpmake: makefile written", write_text(makefile, makebuf, 0644) == 0);

  CHECK("pdpmake: direct binary runs shell-backed recipe",
        run_capture("/bin/pdpmake", pd_argv, outbuf, sizeof(outbuf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
  verify_make_outputs(base, "pdpmake: direct binary");

  cleanup_make_outputs(base);
  CHECK("pdpmake: make symlink runs same recipe",
        run_capture("/bin/make", make_argv, outbuf, sizeof(outbuf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
  verify_make_outputs(base, "pdpmake: /bin/make symlink");

  cleanup_make_outputs(base);
  unlink(makefile);
  rmdir(base);
}

static void test_tcc(void)
{
  const char *base = "/mnt/thirdpartytest-tcc";
  const char *src = "/mnt/thirdpartytest-tcc/prog.c";
  const char *bin = "/mnt/thirdpartytest-tcc/prog";
  const char *data = "/mnt/thirdpartytest-tcc/data.txt";
  char source[4096];
  char buf[1024];
  char *tcc_argv[] = {"tcc", (char *)src, "-o", (char *)bin, NULL};
  char *run_argv[] = {(char *)bin, NULL};
  int status = 0;

  unlink(data);
  unlink(bin);
  unlink(src);
  rmdir(base);

  CHECK("tcc: base dir created", ensure_dir(base) == 0);

  snprintf(source, sizeof(source),
           "#include <fcntl.h>\n"
           "#include <stdio.h>\n"
           "#include <string.h>\n"
           "#include <sys/stat.h>\n"
           "#include <sys/wait.h>\n"
           "#include <unistd.h>\n"
           "int main(void) {\n"
           "  const char *path = \"%s\";\n"
           "  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);\n"
           "  int pfd[2];\n"
           "  pid_t pid;\n"
           "  int status = 0;\n"
           "  char data[8] = {0};\n"
           "  char recv[8] = {0};\n"
           "  struct stat st;\n"
           "  int n;\n"
           "  if (fd < 0) return 2;\n"
           "  if (write(fd, \"ABC\", 3) != 3) return 3;\n"
           "  if (lseek(fd, 0, SEEK_SET) < 0) return 4;\n"
           "  if (read(fd, data, 3) != 3) return 5;\n"
           "  close(fd);\n"
           "  if (pipe(pfd) != 0) return 6;\n"
           "  pid = fork();\n"
           "  if (pid < 0) return 7;\n"
           "  if (pid == 0) {\n"
           "    close(pfd[0]);\n"
           "    write(pfd[1], data, 3);\n"
           "    close(pfd[1]);\n"
           "    _exit(7);\n"
           "  }\n"
           "  close(pfd[1]);\n"
           "  n = read(pfd[0], recv, 3);\n"
           "  close(pfd[0]);\n"
           "  waitpid(pid, &status, 0);\n"
           "  if (stat(path, &st) != 0) return 8;\n"
           "  printf(\"n=%%d child=%%d size=%%ld data=%%s\\n\", n,\n"
           "         WIFEXITED(status) ? WEXITSTATUS(status) : -1,\n"
           "         (long)st.st_size, recv);\n"
           "  return !(n == 3 && WIFEXITED(status) && WEXITSTATUS(status) == 7 &&\n"
           "           st.st_size == 3 && strcmp(recv, \"ABC\") == 0);\n"
           "}\n",
           data);
  CHECK("tcc: source written", write_text(src, source, 0644) == 0);

  CHECK("tcc: compiler builds integration program",
        run_capture("/bin/tcc", tcc_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        access(bin, F_OK) == 0);
  CHECK("tcc: compiled binary runs under SBUnix",
        run_capture(bin, run_argv, buf, sizeof(buf), &status) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        strstr(buf, "n=3 child=7 size=3 data=ABC\n") != NULL);

  unlink(data);
  unlink(bin);
  unlink(src);
  rmdir(base);
}

int main(void)
{
  printf("=== thirdpartytest: vendored workload integration ===\n");

  test_micropython();
  test_busybox();
  test_pdpmake();
  test_tcc();

  printf("thirdpartytest: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
