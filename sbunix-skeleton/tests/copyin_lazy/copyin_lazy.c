/* copyin_lazy — test syscall read from freshly brk'd memory (lazy alloc).
 *
 * Validates that copyin/copyout correctly demand-allocate lazy pages
 * when a syscall accesses memory that was brk'd but never touched
 * from userspace (so no page fault has occurred to map it yet). */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>

int main(void) {
  /* Grow the heap by one page — the kernel uses lazy allocation,
   * so no physical page is mapped until first access. */
  char *buf = sbrk(4096);
  if (buf == (void *)-1) {
    printf("[FAIL] copyin_lazy (sbrk failed)\n");
    return 1;
  }

  /* Create a pipe for testing copyin/copyout through syscalls */
  int pfd[2];
  if (pipe(pfd) < 0) {
    printf("[FAIL] copyin_lazy (pipe failed)\n");
    return 1;
  }

  /* Write a known pattern through the pipe.  The write buffer is on
   * the stack (already mapped), so this tests copyout is working. */
  const char *msg = "lazy_test_ok";
  ssize_t nw = write(pfd[1], msg, strlen(msg));
  if (nw != (ssize_t)strlen(msg)) {
    printf("[FAIL] copyin_lazy (write to pipe failed)\n");
    return 1;
  }

  /* Read from the pipe INTO the freshly brk'd buffer.  This exercises
   * copyin: the kernel must demand-allocate the lazy page for buf
   * before it can copy data there.  Without the vm_resolve_user_page
   * fix, this read would return -1 (EFAULT). */
  ssize_t nr = read(pfd[0], buf, strlen(msg));
  if (nr != (ssize_t)strlen(msg)) {
    printf("[FAIL] copyin_lazy (read into lazy page failed, nr=%ld)\n",
           (long)nr);
    return 1;
  }

  /* Verify the data arrived correctly */
  if (memcmp(buf, msg, strlen(msg)) != 0) {
    printf("[FAIL] copyin_lazy (data mismatch in lazy page)\n");
    return 1;
  }

  /* Also test write FROM the lazy page — this exercises copyout
   * reading from a demand-allocated page. */
  nw = write(pfd[1], buf, strlen(msg));
  if (nw != (ssize_t)strlen(msg)) {
    printf("[FAIL] copyin_lazy (write from lazy page failed)\n");
    return 1;
  }

  /* Read it back to verify */
  char verify[64];
  nr = read(pfd[0], verify, strlen(msg));
  if (nr != (ssize_t)strlen(msg) || memcmp(verify, msg, strlen(msg)) != 0) {
    printf("[FAIL] copyin_lazy (round-trip through lazy page failed)\n");
    return 1;
  }

  close(pfd[0]);
  close(pfd[1]);

  printf("[PASS] copyin_lazy\n");
  return 0;
}
