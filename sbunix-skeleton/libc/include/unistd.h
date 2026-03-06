#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

extern char **environ;

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     open(const char *path, int flags);
int     close(int fd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);
pid_t   fork(void);
pid_t   waitpid(pid_t pid, int *status, int options);
pid_t   getpid(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
void    _exit(int status) __attribute__((noreturn));
unsigned int sleep(unsigned int seconds);

#endif
