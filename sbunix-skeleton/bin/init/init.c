#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <signal.h>

static void sigchld_handler(int sig) {
  (void)sig;
  /* Reap all zombie children (orphaned processes reparented to init) */
  while (waitpid(-1, (int *)0, WNOHANG) > 0)
    ;
}

int main(void) {
  printf("init: SBUnix starting (pid %d)\n", getpid());

  /* Install SIGCHLD handler to reap orphaned zombies */
  struct sigaction sa;
  sa.sa_handler = sigchld_handler;
  sa.sa_mask = 0;
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sa.sa_restorer = 0;
  sigaction(SIGCHLD, &sa, (struct sigaction *)0);

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

  /* Run /etc/rc startup script via sbush */
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {"/bin/sbush", "/etc/rc", (char *)0};
    execve("/bin/sbush", argv, envp);
    _exit(1);
  }
  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
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
    }
    sleep(1); /* prevent tight loop on exec failure */
  }
  return 0;
}
