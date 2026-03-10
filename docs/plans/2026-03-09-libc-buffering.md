# Libc Output Buffering Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add write buffering to libc FILE streams so that `fputc`/`fputs`/`fprintf` don't do a `write()` syscall per character. stdout is line-buffered, stderr remains unbuffered, and file streams are fully buffered.

**Architecture:** Extend the `FILE` struct with a 256-byte write buffer, a buffer position, and a buffering mode flag. Modify `fputc`, `fputs`, `fwrite`, and `fprintf` to write into the buffer. Flush on newline (line-buffered), when full (full-buffered), or on explicit `fflush()`. Wire `exit()` to call `fflush(NULL)` before `_exit()`. Track all open FILE streams so `fflush(NULL)` can flush them all.

**Tech Stack:** C (userspace libc, freestanding, riscv64-unknown-elf-gcc)

---

## Task 1: Extend the FILE struct with a write buffer

**Files:**
- Modify: `libc/include/stdio.h:21-25` (FILE struct)

### Step 1: Add buffer fields to the FILE struct

Replace the FILE typedef (lines 21-25):

```c
typedef struct {
    int fd;
    int eof;
    int error;
} FILE;
```

With:

```c
#define FILE_BUF_SIZE 256

typedef struct _FILE {
    int fd;
    int eof;
    int error;
    int buf_mode;                 /* _IONBF, _IOLBF, or _IOFBF */
    char wbuf[FILE_BUF_SIZE];    /* write buffer */
    int  wpos;                   /* current position in write buffer */
} FILE;
```

Note: The `_IONBF`, `_IOLBF`, `_IOFBF` constants are already defined in stdio.h at lines 12-14.

### Step 2: Build (expect failures — dependent code not yet updated)

Run: `source tools/sourceme.sh && make clean && make`

Expected: Should still compile (new fields are just added; existing code doesn't touch them). The struct is larger but all allocation is static.

### Step 3: Commit

```
ENH: Extended FILE struct with write buffer fields
```

---

## Task 2: Initialize buffering modes for stdin/stdout/stderr

**Files:**
- Modify: `libc/fileio.c:15-22` (static FILE initialization)

### Step 1: Update static FILE initializers

Replace lines 15-22:

```c
/* Static FILE objects for stdin, stdout, stderr */
static FILE __stdin  = { 0, 0, 0 };
static FILE __stdout = { 1, 0, 0 };
static FILE __stderr = { 2, 0, 0 };
```

With:

```c
/* Static FILE objects for stdin, stdout, stderr
 * stdin  — not buffered for writes (it's input-only)
 * stdout — line-buffered (_IOLBF): flush on newline
 * stderr — unbuffered (_IONBF): flush every write immediately
 */
static FILE __stdin  = { .fd = 0, .eof = 0, .error = 0, .buf_mode = _IONBF, .wpos = 0 };
static FILE __stdout = { .fd = 1, .eof = 0, .error = 0, .buf_mode = _IOLBF, .wpos = 0 };
static FILE __stderr = { .fd = 2, .eof = 0, .error = 0, .buf_mode = _IONBF, .wpos = 0 };
```

### Step 2: Update the fopen pool initialization

In `fopen()` (line 150-155), when allocating from the pool, set the new fields:

```c
            __file_pool_used[i] = 1;
            __file_pool[i].fd = fd;
            __file_pool[i].eof = 0;
            __file_pool[i].error = 0;
            __file_pool[i].buf_mode = _IOFBF;  /* file streams: fully buffered */
            __file_pool[i].wpos = 0;
            return &__file_pool[i];
```

### Step 3: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 4: Commit

```
ENH: Initialized buffering modes for standard streams
```

---

## Task 3: Implement buffered `fputc`, `fputs`, and `fwrite`

**Files:**
- Modify: `libc/fileio.c:48-66` (fputc, fputs)
- Modify: `libc/fileio.c:104-113` (fwrite)

### Step 1: Add a `__fflush_stream()` internal helper

Add near the top of `fileio.c` (after the static FILE declarations, before `fprintf`):

```c
/* Internal: flush the write buffer of a single stream */
static int __fflush_stream(FILE *stream) {
    if (!stream || stream->wpos == 0)
        return 0;
    ssize_t r = write(stream->fd, stream->wbuf, stream->wpos);
    if (r < 0) {
        stream->error = 1;
        return EOF;
    }
    stream->wpos = 0;
    return 0;
}
```

### Step 2: Rewrite `fputc` with buffering

Replace the existing `fputc` (lines 58-66):

```c
int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;

    if (stream->buf_mode == _IONBF) {
        /* Unbuffered: write immediately */
        ssize_t r = write(stream->fd, &ch, 1);
        if (r < 0) { stream->error = 1; return EOF; }
        return ch;
    }

    /* Buffered: append to write buffer */
    stream->wbuf[stream->wpos++] = (char)ch;

    /* Flush if buffer full */
    if (stream->wpos >= FILE_BUF_SIZE) {
        if (__fflush_stream(stream) == EOF)
            return EOF;
    }
    /* Line-buffered: also flush on newline */
    else if (stream->buf_mode == _IOLBF && ch == '\n') {
        if (__fflush_stream(stream) == EOF)
            return EOF;
    }

    return ch;
}
```

### Step 3: Rewrite `fputs` with buffering

Replace the existing `fputs` (lines 48-56):

```c
int fputs(const char *s, FILE *stream) {
    int count = 0;
    while (*s) {
        if (fputc((unsigned char)*s, stream) == EOF)
            return EOF;
        s++;
        count++;
    }
    return count;
}
```

This delegates to `fputc` which handles all the buffering logic. For fully-buffered streams, this buffers efficiently because `fputc` only flushes when the buffer is full. For line-buffered, it flushes on each newline. For unbuffered, each char goes out immediately.

### Step 4: Rewrite `fwrite` with buffering

Replace the existing `fwrite` (lines 104-113):

```c
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total = size * nmemb;
    if (total == 0) return 0;
    const unsigned char *p = (const unsigned char *)ptr;
    for (size_t i = 0; i < total; i++) {
        if (fputc(p[i], stream) == EOF)
            return i / size;
    }
    return nmemb;
}
```

### Step 5: Rewrite `fprintf` / `vfprintf` to use buffered output

The current `fprintf` delegates to `vdprintf` which writes directly to the fd. We need it to go through the FILE buffer instead.

Replace `fprintf` and `vfprintf` (lines 29-39):

```c
int fprintf(FILE *stream, const char *fmt, ...) {
    /* Format into a temporary buffer, then write through the FILE */
    static char __fprintf_buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(__fprintf_buf, sizeof(__fprintf_buf), fmt, ap);
    va_end(ap);
    if (len > 0) {
        for (int i = 0; i < len; i++) {
            if (fputc((unsigned char)__fprintf_buf[i], stream) == EOF)
                return -1;
        }
    }
    return len;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    static char __vfprintf_buf[1024];
    int len = vsnprintf(__vfprintf_buf, sizeof(__vfprintf_buf), fmt, ap);
    if (len > 0) {
        for (int i = 0; i < len; i++) {
            if (fputc((unsigned char)__vfprintf_buf[i], stream) == EOF)
                return -1;
        }
    }
    return len;
}
```

### Step 6: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 7: Commit

```
ENH: Added buffered writes to fputc, fputs, fwrite, and fprintf
```

---

## Task 4: Implement `fflush()` and `fflush(NULL)`

**Files:**
- Modify: `libc/fileio.c:178-181` (fflush)

### Step 1: Track all open FILE streams

Add a registry to track open streams. Near the top of `fileio.c`, after the static FILE declarations:

```c
/* Registry of all open FILE streams (for fflush(NULL)) */
#define MAX_OPEN_STREAMS (3 + FILE_POOL_SIZE)
static FILE *__open_streams[MAX_OPEN_STREAMS];
static int __nopen_streams = 0;

static void __register_stream(FILE *stream) {
    for (int i = 0; i < __nopen_streams; i++)
        if (__open_streams[i] == stream)
            return;  /* already registered */
    if (__nopen_streams < MAX_OPEN_STREAMS)
        __open_streams[__nopen_streams++] = stream;
}

static void __unregister_stream(FILE *stream) {
    for (int i = 0; i < __nopen_streams; i++) {
        if (__open_streams[i] == stream) {
            __open_streams[i] = __open_streams[--__nopen_streams];
            return;
        }
    }
}
```

Register stdin/stdout/stderr on first use. The simplest approach: add an init function that's called early, or register them statically. Since we can't have constructors in freestanding, register them in a lazy initializer or just add them to an `__init_streams()` function.

Add:

```c
static int __streams_inited = 0;
static void __init_streams(void) {
    if (__streams_inited) return;
    __streams_inited = 1;
    __register_stream(stdin);
    __register_stream(stdout);
    __register_stream(stderr);
}
```

Call `__init_streams()` at the top of `fflush()`, `fopen()`, `fputc()` — or better, just once in `fflush()` since that's where `NULL` matters.

### Step 2: Rewrite `fflush()`

Replace the existing stub (lines 178-181):

```c
int fflush(FILE *stream) {
    if (stream)
        return __fflush_stream(stream);

    /* fflush(NULL): flush all open streams */
    __init_streams();
    int ret = 0;
    for (int i = 0; i < __nopen_streams; i++) {
        if (__fflush_stream(__open_streams[i]) == EOF)
            ret = EOF;
    }
    return ret;
}
```

### Step 3: Register/unregister in fopen/fclose

In `fopen()`, after allocating and initializing the FILE, add:

```c
            __init_streams();
            __register_stream(&__file_pool[i]);
```

In `fclose()`, before returning, add:

```c
    __fflush_stream(stream); /* flush before closing */
    __unregister_stream(stream);
```

### Step 4: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 5: Commit

```
ENH: Implemented fflush with support for flushing all streams
```

---

## Task 5: Wire `exit()` to flush streams

**Files:**
- Modify: `libc/exit.c` (call fflush(NULL) before _exit)

### Step 1: Update exit() to flush

Replace the current `exit()`:

```c
#include <stdlib.h>
#include <syscall.h>

void exit(int status) {
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}
```

With:

```c
#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>

void exit(int status) {
    fflush(NULL);  /* flush all buffered output before exiting */
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}
```

### Step 2: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 3: Commit

```
ENH: Wired exit to flush all buffered streams before termination
```

---

## Task 6: Update `setvbuf` to actually work

**Files:**
- Modify: `libc/fileio.c:222-225` (setvbuf)

### Step 1: Replace the setvbuf stub

Replace:

```c
int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0; /* unbuffered — always succeeds */
}
```

With:

```c
int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)buf; (void)size;
    /* We use the internal buffer only — ignore external buf/size */
    if (mode == _IONBF || mode == _IOLBF || mode == _IOFBF) {
        __fflush_stream(stream);  /* flush any pending data under old mode */
        stream->buf_mode = mode;
        return 0;
    }
    return -1;  /* invalid mode */
}
```

### Step 2: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 3: Commit

```
ENH: Implemented setvbuf for changing stream buffering mode
```

---

## Task 7: End-to-end test

### Step 1: Write a test program

Create `bin/buftest/buftest.c`:

```c
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    /* Print 100 lines through buffered stdout */
    for (int i = 0; i < 100; i++) {
        printf("line %d: hello from buffered output\n", i);
    }

    /* Test stderr (unbuffered) */
    fprintf(stderr, "stderr: this should appear immediately\n");

    /* Test fflush */
    printf("before flush...");
    fflush(stdout);
    printf(" after flush\n");

    return 0;
}
```

### Step 2: Build and test

Run: `source tools/sourceme.sh && make clean && make && make qemu`

In the shell:
```
sbush$ buftest
line 0: hello from buffered output
line 1: hello from buffered output
...
line 99: hello from buffered output
stderr: this should appear immediately
before flush... after flush
```

**What to verify:**
1. All 100 lines appear, in order, not garbled
2. stderr output appears (mixed in correctly)
3. "before flush..." and "after flush" appear on the same line (the `fflush` forced the partial line out)
4. No truncation or lost output
5. The program exits cleanly (exit() flushes remaining buffers)

### Step 3: Remove or keep the test program

The test program can stay in `bin/buftest/` as a utility, or be removed. If keeping, commit:

```
TEST: Added buffer output test program
```

If removing, just delete it after verifying.
