#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <signal.h>

static void reap_orphans(void) {
  while (waitpid(-1, (int *)0, WNOHANG) > 0)
    ;
}

int main(void) {
  printf("init: SBUnix starting (pid %d)\n", getpid());

  /* Enriched environment for child processes */
  char *envp[] = {
    "PATH=/bin",
    "HOME=/",
    "TERM=vt100",
    "HOSTNAME=sbunix",
    "PS1=sbush$ ",
    "SHELL=/bin/sbush",
    (char *)0
  };

  char *rc_envp[] = {
    "PATH=/bin",
    "HOME=/",
    "TERM=vt100",
    "HOSTNAME=sbunix",
    "PS1=sbush$ ",
    "SHELL=/bin/sbush",
    "SBUNIX_BOOT_RC=1",
    (char *)0
  };

  /* Run /etc/rc startup script via sbush */
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {"/bin/sbush", "/etc/rc", (char *)0};
    execve("/bin/sbush", argv, rc_envp);
    _exit(1);
  }
  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    reap_orphans();
  }
  /* Respawn interactive shell forever */
  while (1) {
    pid = fork();
    if (pid == 0) {
      setsid(); /* new session for the shell */
      char *argv[] = {"/bin/sbush", (char *)0};
      execve("/bin/sbush", argv, envp);
      _exit(1);
    }
    if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
      printf("init: shell exited, respawning...\n");
      reap_orphans();
    }
    sleep(1); /* prevent tight loop on exec failure */
  }
  return 0;
}
