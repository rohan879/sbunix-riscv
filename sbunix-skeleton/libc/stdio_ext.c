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
  if (!str || !fmt)
    return EOF;

  const char *s = str;
  const char *f = fmt;
  int matched = 0;

#define SS_ISSPACE(c) ((c)==' '||(c)=='\t'||(c)=='\n'||(c)=='\r'||(c)=='\f'||(c)=='\v')
#define SS_ISDIGIT(c) ((c)>='0'&&(c)<='9')
#define SS_ISXDIGIT(c) (SS_ISDIGIT(c)||((c)>='a'&&(c)<='f')||((c)>='A'&&(c)<='F'))
#define SS_ISODIGIT(c) ((c)>='0'&&(c)<='7')

  while (*f) {
    if (SS_ISSPACE((unsigned char)*f)) {
      while (SS_ISSPACE((unsigned char)*s)) s++;
      while (SS_ISSPACE((unsigned char)*f)) f++;
      continue;
    }

    if (*f != '%') {
      if (*s != *f) goto done;
      s++; f++;
      continue;
    }

    f++;
    if (*f == '%') {
      if (*s != '%') goto done;
      s++; f++;
      continue;
    }

    int suppress = 0;
    if (*f == '*') { suppress = 1; f++; }

    int width = 0, has_width = 0;
    while (SS_ISDIGIT((unsigned char)*f)) {
      width = width * 10 + (*f - '0');
      has_width = 1;
      f++;
    }

    int length = 0;
    if (*f == 'l') {
      f++; length = 1;
      if (*f == 'l') { f++; length = 2; }
    } else if (*f == 'h') {
      f++; length = -1;
      if (*f == 'h') { f++; length = -2; }
    }

    char spec = *f++;

    switch (spec) {
    case 'd': case 'i': {
      while (SS_ISSPACE((unsigned char)*s)) s++;
      if (!*s) goto done;
      const char *start = s;
      int neg = 0;
      if (*s == '-' || *s == '+') {
        neg = (*s == '-'); s++;
        if (has_width) width--;
      }
      if (!SS_ISDIGIT((unsigned char)*s)) { s = start; goto done; }
      long long val = 0;
      int count = 0;
      while (SS_ISDIGIT((unsigned char)*s) && (!has_width || count < width)) {
        val = val * 10 + (*s - '0'); s++; count++;
      }
      if (neg) val = -val;
      if (!suppress) {
        if (length == 2) *va_arg(ap, long long *) = val;
        else if (length == 1) *va_arg(ap, long *) = (long)val;
        else if (length == -1) *va_arg(ap, short *) = (short)val;
        else if (length == -2) *va_arg(ap, signed char *) = (signed char)val;
        else *va_arg(ap, int *) = (int)val;
        matched++;
      }
      break;
    }
    case 'u': {
      while (SS_ISSPACE((unsigned char)*s)) s++;
      if (!*s) goto done;
      const char *start = s;
      if (*s == '+') { s++; if (has_width) width--; }
      if (!SS_ISDIGIT((unsigned char)*s)) { s = start; goto done; }
      unsigned long long val = 0;
      int count = 0;
      while (SS_ISDIGIT((unsigned char)*s) && (!has_width || count < width)) {
        val = val * 10 + (unsigned)(*s - '0'); s++; count++;
      }
      if (!suppress) {
        if (length == 2) *va_arg(ap, unsigned long long *) = val;
        else if (length == 1) *va_arg(ap, unsigned long *) = (unsigned long)val;
        else if (length == -1) *va_arg(ap, unsigned short *) = (unsigned short)val;
        else if (length == -2) *va_arg(ap, unsigned char *) = (unsigned char)val;
        else *va_arg(ap, unsigned int *) = (unsigned int)val;
        matched++;
      }
      break;
    }
    case 'x': case 'X': {
      while (SS_ISSPACE((unsigned char)*s)) s++;
      if (!*s) goto done;
      const char *start = s;
      if (*s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) {
        s += 2; if (has_width) width -= 2;
      }
      if (!SS_ISXDIGIT((unsigned char)*s)) { s = start; goto done; }
      unsigned long long val = 0;
      int count = 0;
      while (SS_ISXDIGIT((unsigned char)*s) && (!has_width || count < width)) {
        unsigned char c = (unsigned char)*s;
        unsigned digit = (c >= '0' && c <= '9') ? c - '0' :
                         (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
        val = val * 16 + digit; s++; count++;
      }
      if (!suppress) {
        if (length == 2) *va_arg(ap, unsigned long long *) = val;
        else if (length == 1) *va_arg(ap, unsigned long *) = (unsigned long)val;
        else *va_arg(ap, unsigned int *) = (unsigned int)val;
        matched++;
      }
      break;
    }
    case 'o': {
      while (SS_ISSPACE((unsigned char)*s)) s++;
      if (!*s) goto done;
      if (!SS_ISODIGIT((unsigned char)*s)) goto done;
      unsigned long long val = 0;
      int count = 0;
      while (SS_ISODIGIT((unsigned char)*s) && (!has_width || count < width)) {
        val = val * 8 + (unsigned)(*s - '0'); s++; count++;
      }
      if (!suppress) {
        if (length == 2) *va_arg(ap, unsigned long long *) = val;
        else if (length == 1) *va_arg(ap, unsigned long *) = (unsigned long)val;
        else *va_arg(ap, unsigned int *) = (unsigned int)val;
        matched++;
      }
      break;
    }
    case 's': {
      while (SS_ISSPACE((unsigned char)*s)) s++;
      if (!*s) goto done;
      int count = 0;
      if (!suppress) {
        char *dest = va_arg(ap, char *);
        while (*s && !SS_ISSPACE((unsigned char)*s) && (!has_width || count < width)) {
          *dest++ = *s++; count++;
        }
        *dest = '\0';
        matched++;
      } else {
        while (*s && !SS_ISSPACE((unsigned char)*s) && (!has_width || count < width)) {
          s++; count++;
        }
      }
      break;
    }
    case 'c': {
      int w = has_width ? width : 1;
      if (!suppress) {
        char *dest = va_arg(ap, char *);
        for (int i = 0; i < w; i++) {
          if (!*s) goto done;
          *dest++ = *s++;
        }
        matched++;
      } else {
        for (int i = 0; i < w; i++) {
          if (!*s) goto done;
          s++;
        }
      }
      break;
    }
    case 'n': {
      if (!suppress) {
        if (length == 2) *va_arg(ap, long long *) = (long long)(s - str);
        else if (length == 1) *va_arg(ap, long *) = (long)(s - str);
        else *va_arg(ap, int *) = (int)(s - str);
      }
      break;
    }
    case '[': {
      int negate = 0;
      if (*f == '^') { negate = 1; f++; }
      unsigned char charset[32];
      for (int k = 0; k < 32; k++) charset[k] = 0;
      if (*f == ']') {
        charset[(unsigned char)']' / 8] |= (unsigned char)(1 << ((unsigned char)']' % 8));
        f++;
      }
      while (*f && *f != ']') {
        unsigned char lo = (unsigned char)*f;
        if (*(f+1) == '-' && *(f+2) && *(f+2) != ']') {
          unsigned char hi = (unsigned char)*(f+2);
          for (unsigned char c = lo; c <= hi; c++)
            charset[c / 8] |= (unsigned char)(1 << (c % 8));
          f += 3;
        } else {
          charset[lo / 8] |= (unsigned char)(1 << (lo % 8));
          f++;
        }
      }
      if (*f == ']') f++;
      int count = 0;
      if (!suppress) {
        char *dest = va_arg(ap, char *);
        while (*s && (!has_width || count < width)) {
          unsigned char c = (unsigned char)*s;
          int in_set = (charset[c / 8] >> (c % 8)) & 1;
          if (negate ? in_set : !in_set) break;
          *dest++ = *s++; count++;
        }
        *dest = '\0';
      } else {
        while (*s && (!has_width || count < width)) {
          unsigned char c = (unsigned char)*s;
          int in_set = (charset[c / 8] >> (c % 8)) & 1;
          if (negate ? in_set : !in_set) break;
          s++; count++;
        }
      }
      if (count == 0) goto done;
      if (!suppress) matched++;
      break;
    }
    default:
      goto done;
    }
  }

done:
#undef SS_ISSPACE
#undef SS_ISDIGIT
#undef SS_ISXDIGIT
#undef SS_ISODIGIT
  return matched;
}

/* BF: vfscanf previously read an entire line with fgets, then scanned it.
 * This consumed characters beyond what the format matched, breaking
 * subsequent reads on the same stream.  Now reads character-by-character
 * into a local buffer (line-at-a-time) via fgetc, so the read buffer
 * and ungetc state are properly respected. */
int vfscanf(FILE *stream, const char *fmt, va_list ap) {
  char buf[4096];
  int pos = 0;
  while (pos < (int)sizeof(buf) - 1) {
    int c = fgetc(stream);
    if (c == EOF) break;
    buf[pos++] = (char)c;
    if (c == '\n') break;
  }
  buf[pos] = '\0';
  if (pos == 0) return EOF;
  int result = vsscanf(buf, fmt, ap);
  return result;
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
