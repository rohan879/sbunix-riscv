#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <syscall.h>

typedef struct {
  FILE *fp;
  int type;
  char *fmem_buf;
  size_t fmem_size;
  int fmem_writable;
  char **memptr;
  size_t *memsizeloc;
  wchar_t **wmemptr;
  size_t *wmemsizeloc;
} memstream_state_t;

enum {
  MS_NONE = 0,
  MS_FMEM = 1,
  MS_MEM = 2,
  MS_WMEM = 3,
};

#define MEMSTREAM_MAX 32
static memstream_state_t g_memstreams[MEMSTREAM_MAX];
static unsigned int g_tmpnam_counter;

static memstream_state_t *find_memstream(FILE *fp) {
  for (int i = 0; i < MEMSTREAM_MAX; i++) {
    if (g_memstreams[i].fp == fp)
      return &g_memstreams[i];
  }
  return (void *)0;
}

static memstream_state_t *alloc_memstream(FILE *fp) {
  for (int i = 0; i < MEMSTREAM_MAX; i++) {
    if (!g_memstreams[i].fp) {
      g_memstreams[i].fp = fp;
      return &g_memstreams[i];
    }
  }
  return (void *)0;
}

static void sync_memstream(memstream_state_t *ms) {
  if (!ms || !ms->fp)
    return;

  if (ms->type == MS_FMEM) {
    if (!ms->fmem_writable || !ms->fmem_buf || ms->fmem_size == 0)
      return;
    (void)fseek(ms->fp, 0, SEEK_SET);
    size_t n = fread(ms->fmem_buf, 1, ms->fmem_size, ms->fp);
    if (n < ms->fmem_size)
      memset(ms->fmem_buf + n, 0, ms->fmem_size - n);
    return;
  }

  if (ms->type != MS_MEM && ms->type != MS_WMEM)
    return;

  if (fseek(ms->fp, 0, SEEK_END) < 0)
    return;
  long end = ftell(ms->fp);
  if (end < 0)
    return;
  if (fseek(ms->fp, 0, SEEK_SET) < 0)
    return;

  size_t len = (size_t)end;
  char *buf = (char *)malloc(len + 1);
  if (!buf)
    return;

  size_t nread = fread(buf, 1, len, ms->fp);
  buf[nread] = '\0';

  if (ms->type == MS_MEM) {
    if (ms->memptr && *ms->memptr)
      free(*ms->memptr);
    if (ms->memptr)
      *ms->memptr = buf;
    else
      free(buf);
    if (ms->memsizeloc)
      *ms->memsizeloc = nread;
    return;
  }

  wchar_t *wbuf = (wchar_t *)malloc((nread + 1) * sizeof(wchar_t));
  if (!wbuf) {
    free(buf);
    return;
  }
  for (size_t i = 0; i < nread; i++)
    wbuf[i] = (unsigned char)buf[i];
  wbuf[nread] = 0;
  free(buf);

  if (ms->wmemptr && *ms->wmemptr)
    free(*ms->wmemptr);
  if (ms->wmemptr)
    *ms->wmemptr = wbuf;
  else
    free(wbuf);
  if (ms->wmemsizeloc)
    *ms->wmemsizeloc = nread;
}

void __sbunix_memstream_sync(FILE *stream) {
  memstream_state_t *ms = find_memstream(stream);
  if (ms)
    sync_memstream(ms);
}

void __sbunix_memstream_close(FILE *stream) {
  memstream_state_t *ms = find_memstream(stream);
  if (!ms)
    return;
  sync_memstream(ms);
  memset(ms, 0, sizeof(*ms));
}

int fseeko(FILE *stream, off_t offset, int whence) {
  return fseek(stream, (long)offset, whence);
}

off_t ftello(FILE *stream) {
  return (off_t)ftell(stream);
}

int fgetpos(FILE *stream, fpos_t *pos) {
  if (!stream || !pos)
    return -1;
  long off = ftell(stream);
  if (off < 0)
    return -1;
  *pos = (fpos_t)off;
  return 0;
}

int fsetpos(FILE *stream, const fpos_t *pos) {
  if (!stream || !pos)
    return -1;
  return fseek(stream, (long)*pos, SEEK_SET);
}

void flockfile(FILE *stream) { (void)stream; }
int ftrylockfile(FILE *stream) { (void)stream; return 0; }
void funlockfile(FILE *stream) { (void)stream; }

int getc_unlocked(FILE *stream) { return getc(stream); }
int getchar_unlocked(void) { return getchar(); }
int putc_unlocked(int c, FILE *stream) { return putc(c, stream); }
int putchar_unlocked(int c) { return putchar(c); }

char *tmpnam(char *s) {
  static char buf[L_tmpnam];
  char *out = s ? s : buf;
  unsigned int id = ++g_tmpnam_counter;
  snprintf(out, L_tmpnam, "/tmp/tmpnam%u", id);
  return out;
}

char *ctermid(char *s) {
  static char term[L_ctermid] = "/dev/console";
  if (!s)
    return term;
  strncpy(s, term, L_ctermid - 1);
  s[L_ctermid - 1] = '\0';
  return s;
}

FILE *fmemopen(void *buf, size_t size, const char *mode) {
  if (!mode) {
    errno = EINVAL;
    return (void *)0;
  }

  FILE *fp = tmpfile();
  if (!fp)
    return (void *)0;

  int writable = (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) ? 1 : 0;
  int readable = (strchr(mode, 'r') || strchr(mode, '+')) ? 1 : 0;

  if (readable && buf && size > 0) {
    (void)fwrite(buf, 1, size, fp);
    (void)fseek(fp, 0, SEEK_SET);
  }
  if (mode[0] == 'a')
    (void)fseek(fp, 0, SEEK_END);

  memstream_state_t *ms = alloc_memstream(fp);
  if (!ms) {
    fclose(fp);
    errno = ENOMEM;
    return (void *)0;
  }
  ms->type = MS_FMEM;
  ms->fmem_buf = (char *)buf;
  ms->fmem_size = size;
  ms->fmem_writable = writable;
  return fp;
}

FILE *open_memstream(char **ptr, size_t *sizeloc) {
  if (!ptr || !sizeloc) {
    errno = EINVAL;
    return (void *)0;
  }
  FILE *fp = tmpfile();
  if (!fp)
    return (void *)0;
  *ptr = (void *)0;
  *sizeloc = 0;

  memstream_state_t *ms = alloc_memstream(fp);
  if (!ms) {
    fclose(fp);
    errno = ENOMEM;
    return (void *)0;
  }
  ms->type = MS_MEM;
  ms->memptr = ptr;
  ms->memsizeloc = sizeloc;
  return fp;
}

FILE *open_wmemstream(wchar_t **ptr, size_t *sizeloc) {
  if (!ptr || !sizeloc) {
    errno = EINVAL;
    return (void *)0;
  }
  FILE *fp = tmpfile();
  if (!fp)
    return (void *)0;
  *ptr = (void *)0;
  *sizeloc = 0;

  memstream_state_t *ms = alloc_memstream(fp);
  if (!ms) {
    fclose(fp);
    errno = ENOMEM;
    return (void *)0;
  }
  ms->type = MS_WMEM;
  ms->wmemptr = ptr;
  ms->wmemsizeloc = sizeloc;
  return fp;
}

int vasprintf(char **strp, const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int needed = vsnprintf((void *)0, 0, fmt, ap2);
  va_end(ap2);
  if (needed < 0)
    return -1;
  char *buf = (char *)malloc((size_t)needed + 1);
  if (!buf)
    return -1;
  int out = vsnprintf(buf, (size_t)needed + 1, fmt, ap);
  if (out < 0) {
    free(buf);
    return -1;
  }
  *strp = buf;
  return out;
}

int asprintf(char **strp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vasprintf(strp, fmt, ap);
  va_end(ap);
  return r;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
  (void)str;
  (void)fmt;
  (void)ap;
  errno = ENOSYS;
  return 0;
}

int vfscanf(FILE *stream, const char *fmt, va_list ap) {
  char buf[4096];
  if (!fgets(buf, sizeof(buf), stream))
    return EOF;
  return vsscanf(buf, fmt, ap);
}

int fscanf(FILE *stream, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfscanf(stream, fmt, ap);
  va_end(ap);
  return r;
}

int vscanf(const char *fmt, va_list ap) {
  return vfscanf(stdin, fmt, ap);
}

int scanf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfscanf(stdin, fmt, ap);
  va_end(ap);
  return r;
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
  return (int)__syscall_ret(syscall(SYS_renameat2, olddirfd, oldpath, newdirfd, newpath, 0));
}
