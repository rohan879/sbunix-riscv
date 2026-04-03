#pragma once

typedef unsigned long size_t;
typedef long          ssize_t;
typedef long          ptrdiff_t;

#ifndef __cplusplus
typedef int wchar_t;
#endif

typedef struct {
  long long __max_align_ll;
  long double __max_align_ld;
} max_align_t;

#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)
