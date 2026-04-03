/*
 * dlfcn.c — dynamic linking interface for SBUnix libc.
 *
 * SBUnix uses static linking only (no shared libraries, no dynamic linker).
 * This implementation provides dlopen/dlsym/dlclose/dlerror that work with
 * a built-in symbol table of libc functions.
 *
 * dlopen(NULL, ...) or dlopen with RTLD_DEFAULT returns a handle to the
 * current process image. dlsym() looks up symbols from a compiled-in
 * table of libc function pointers. This allows programs like TCC to
 * resolve libc symbols at runtime without a dynamic linker.
 */

#include <dlfcn.h>
#include <string.h>
#include <stddef.h>

/* Forward declarations of libc functions for the symbol table */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fnmatch.h>
#include <setjmp.h>

/* Thread-safe error string */
static char dl_errbuf[128];
static int  dl_err_set;

/* Pseudo-handle for the current process */
static int dl_self_handle = 1;

/* Symbol table entry */
struct dl_symbol {
    const char *name;
    void       *addr;
};

/* Built-in symbol table — maps names to function pointers.
 * This allows dlsym(RTLD_DEFAULT, "printf") to return &printf. */
#define SYM(fn) { #fn, (void *)(fn) }

static const struct dl_symbol dl_symtab[] = {
    /* stdio */
    SYM(printf), SYM(fprintf), SYM(sprintf), SYM(snprintf),
    SYM(vprintf), SYM(vfprintf), SYM(vsprintf), SYM(vsnprintf),
    SYM(dprintf), SYM(vdprintf),
    SYM(fopen), SYM(fclose), SYM(fread), SYM(fwrite),
    SYM(fgets), SYM(fgetc), SYM(fputc), SYM(fputs),
    SYM(fflush), SYM(feof), SYM(ferror), SYM(clearerr),
    SYM(fseek), SYM(ftell), SYM(rewind),
    SYM(fileno), SYM(fdopen), SYM(tmpfile),
    SYM(popen), SYM(pclose),
    SYM(sscanf), SYM(perror), SYM(ungetc),
    SYM(puts), SYM(putchar), SYM(getchar),
    SYM(setvbuf), SYM(setbuf),
    SYM(getc), SYM(putc),

    /* stdlib */
    SYM(malloc), SYM(free), SYM(calloc), SYM(realloc),
    SYM(exit), SYM(abort), SYM(atexit),
    SYM(atoi), SYM(atol), SYM(strtol), SYM(strtoul),
    SYM(strtoll), SYM(strtoull), SYM(strtod), SYM(strtof),
    SYM(getenv), SYM(setenv), SYM(unsetenv), SYM(putenv),
    SYM(system), SYM(mkstemp), SYM(realpath), SYM(remove),
    SYM(qsort), SYM(bsearch), SYM(abs), SYM(labs),
    SYM(rand), SYM(srand),

    /* string */
    SYM(strlen), SYM(strcmp), SYM(strncmp),
    SYM(strcpy), SYM(strncpy), SYM(strcat), SYM(strncat),
    SYM(strchr), SYM(strrchr), SYM(strstr), SYM(strdup), SYM(strndup),
    SYM(memcpy), SYM(memset), SYM(memcmp), SYM(memmove), SYM(memchr),
    SYM(strerror), SYM(strcasecmp), SYM(strncasecmp),
    SYM(strspn), SYM(strcspn), SYM(strpbrk), SYM(strtok),

    /* unistd */
    SYM(read), SYM(write), SYM(open), SYM(close),
    SYM(lseek), SYM(dup), SYM(dup2), SYM(pipe),
    SYM(fork), SYM(execve), SYM(execv), SYM(execvp),
    SYM(getpid), SYM(getppid), SYM(getcwd), SYM(chdir),
    SYM(unlink), SYM(rmdir), SYM(link), SYM(rename),
    SYM(access), SYM(isatty), SYM(sleep), SYM(usleep),
    SYM(readlink), SYM(symlink),
    SYM(ftruncate), SYM(fsync), SYM(sync),
    SYM(waitpid), SYM(wait), SYM(_exit),
    SYM(setsid), SYM(setpgid), SYM(getpgrp),

    /* fcntl */
    SYM(fcntl), SYM(openat), SYM(creat),

    /* signal */
    SYM(signal), SYM(sigaction), SYM(kill), SYM(raise),

    /* stat */
    SYM(stat), SYM(fstat), SYM(lstat), SYM(mkdir), SYM(umask),

    /* mman */
    SYM(mmap), SYM(munmap), SYM(mprotect),

    /* dirent */
    SYM(opendir), SYM(readdir), SYM(closedir),

    /* time */
    SYM(time), SYM(clock_gettime), SYM(nanosleep),
    SYM(gmtime), SYM(localtime), SYM(mktime), SYM(strftime),

    /* setjmp */
    SYM(setjmp), SYM(longjmp),

    /* errno */
    { "errno", (void *)&__errno_value },

    /* sentinel */
    { NULL, NULL }
};

#undef SYM

/*
 * dlopen — open a "shared library".
 * In SBUnix (static linking only):
 *   - NULL or "" returns a handle to the current process (self)
 *   - Any other filename sets an error (no .so loading support)
 */
void *dlopen(const char *filename, int flags) {
    (void)flags;
    if (filename == NULL || filename[0] == '\0') {
        dl_err_set = 0;
        return (void *)&dl_self_handle;
    }
    /* No dynamic library loading — set error */
    strcpy(dl_errbuf, "dlopen: shared libraries not supported");
    dl_err_set = 1;
    return NULL;
}

/*
 * dlsym — look up a symbol by name.
 * Searches the built-in symbol table for a matching name.
 */
void *dlsym(void *handle, const char *symbol) {
    (void)handle;
    if (!symbol) {
        strcpy(dl_errbuf, "dlsym: NULL symbol name");
        dl_err_set = 1;
        return NULL;
    }

    for (const struct dl_symbol *s = dl_symtab; s->name; s++) {
        if (strcmp(s->name, symbol) == 0) {
            dl_err_set = 0;
            return s->addr;
        }
    }

    /* Symbol not found */
    snprintf(dl_errbuf, sizeof(dl_errbuf),
             "dlsym: undefined symbol: %s", symbol);
    dl_err_set = 1;
    return NULL;
}

/*
 * dlclose — close a "shared library" handle.
 * Always succeeds (nothing to unload in static linking).
 */
int dlclose(void *handle) {
    (void)handle;
    dl_err_set = 0;
    return 0;
}

/*
 * dlerror — return last error string, clear error state.
 */
char *dlerror(void) {
    if (dl_err_set) {
        dl_err_set = 0;
        return dl_errbuf;
    }
    return NULL;
}
