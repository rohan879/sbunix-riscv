#pragma once
#include_next <sys/time.h>

int settimeofday(const struct timeval *tv, const struct timezone *tz);
int utimes(const char *path, const struct timeval times[2]);
