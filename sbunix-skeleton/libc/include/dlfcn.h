#ifndef _DLFCN_H
#define _DLFCN_H

/* dlopen mode flags */
#define RTLD_LAZY     0x0001
#define RTLD_NOW      0x0002
#define RTLD_GLOBAL   0x0100
#define RTLD_LOCAL    0x0000
#define RTLD_NOLOAD   0x0004
#define RTLD_NODELETE 0x1000

/* Special handle for dlsym */
#define RTLD_DEFAULT  ((void *)0)
#define RTLD_NEXT     ((void *)-1L)

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int   dlclose(void *handle);
char *dlerror(void);

#endif /* _DLFCN_H */
