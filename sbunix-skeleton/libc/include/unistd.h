#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* access() mode bits */
#define F_OK  0  /* test for file existence */
#define X_OK  1  /* test for execute permission */
#define W_OK  2  /* test for write permission */
#define R_OK  4  /* test for read permission */

/* lseek() whence values */
#define SEEK_SET  0  /* seek from beginning of file */
#define SEEK_CUR  1  /* seek from current position */
#define SEEK_END  2  /* seek from end of file */

extern char **environ;

/* Basic I/O */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     open(const char *path, int flags, ...);
int     close(int fd);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);
off_t   lseek(int fd, off_t offset, int whence);

/* Process management */
pid_t   fork(void);
pid_t   getpid(void);
pid_t   getppid(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
pid_t   waitpid(pid_t pid, int *status, int options);
void    _exit(int status) __attribute__((noreturn));

/* Filesystem */
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     unlink(const char *path);
int     rmdir(const char *path);
int     link(const char *oldpath, const char *newpath);
int     rename(const char *oldpath, const char *newpath);
int     access(const char *path, int mode);

/* Time */
unsigned int sleep(unsigned int seconds);
int          usleep(unsigned int usecs);
unsigned int alarm(unsigned int seconds);

/* Miscellaneous */
uid_t   getuid(void);
gid_t   getgid(void);
uid_t   geteuid(void);
gid_t   getegid(void);

/* Terminal / process group */
int     isatty(int fd);
pid_t   setsid(void);
pid_t   getpgrp(void);
int     setpgid(pid_t pid, pid_t pgid);
pid_t   tcgetpgrp(int fd);
int     tcsetpgrp(int fd, pid_t pgrp);
pid_t   getpgid(pid_t pid);
pid_t   getsid(pid_t pid);

/* File operations */
int     ftruncate(int fd, off_t length);

/* Wait (convenience wrapper) */
pid_t   wait(int *status);

#endif /* _UNISTD_H */
