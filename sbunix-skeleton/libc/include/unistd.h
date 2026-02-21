#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

ssize_t write(int fildes, const void *buf, size_t nbyte);
ssize_t read(int fildes, void *buf, size_t nbyte);
void _exit(int status) __attribute__((noreturn));

#endif
