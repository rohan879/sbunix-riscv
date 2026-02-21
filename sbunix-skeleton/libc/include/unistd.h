#ifndef _UNISTD_H
#define _UNISTD_H

#include <riscv.h> // For size_t

int write(int fd, const char *buf, int len);
int read(int fd, char *buf, int len);

#endif
