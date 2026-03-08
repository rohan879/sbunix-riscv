#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>

int main(void) {
  /* Run /etc/rc startup script via sbush */
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {"/bin/sbush", "/etc/rc", (char *)0};
    char *envp[] = {"PATH=/bin", (char *)0};
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
      char *argv[] = {"/bin/sbush", (char *)0};
      char *envp[] = {"PATH=/bin", "PS1=sbush$ ", (char *)0};
      execve("/bin/sbush", argv, envp);
      _exit(1);
    }
    if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
    }
  }
  return 0;
}
