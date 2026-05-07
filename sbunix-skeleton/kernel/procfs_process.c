#include <procfs_internal.h>
#include <string.h>
#include <vfs.h>
#include <vm.h>
#include <errno.h>

static int append_char(char *buf, int bufsz, int pos, char c) {
  if (pos < bufsz)
    buf[pos++] = c;
  return pos;
}

static int append_str(char *buf, int bufsz, int pos, const char *s) {
  int len = strlen(s);
  if (pos >= bufsz)
    return pos;
  if (pos + len > bufsz)
    len = bufsz - pos;
  if (len > 0) {
    memcpy(buf + pos, s, len);
    pos += len;
  }
  return pos;
}

static int append_int(char *buf, int bufsz, int pos, long v) {
  char tmp[24];
  int ti = 0;

  if (v < 0) {
    pos = append_char(buf, bufsz, pos, '-');
    v = -v;
  }
  if (v == 0) {
    tmp[ti++] = '0';
  } else {
    while (v > 0 && ti < (int)sizeof(tmp)) {
      tmp[ti++] = (char)('0' + (v % 10));
      v /= 10;
    }
  }
  while (ti > 0)
    pos = append_char(buf, bufsz, pos, tmp[--ti]);
  return pos;
}

static int append_octal(char *buf, int bufsz, int pos, unsigned int v) {
  char tmp[16];
  int ti = 0;

  tmp[ti++] = '0';
  if (v == 0) {
    tmp[ti++] = '0';
  } else {
    char rev[12];
    int ri = 0;
    while (v > 0 && ri < (int)sizeof(rev)) {
      rev[ri++] = (char)('0' + (v & 7));
      v >>= 3;
    }
    while (ri > 0 && ti < (int)sizeof(tmp))
      tmp[ti++] = rev[--ri];
  }
  for (int i = 0; i < ti; i++)
    pos = append_char(buf, bufsz, pos, tmp[i]);
  return pos;
}

static uint64_t statm_vmsize_pages(struct proc *p) {
  uint64_t total = p ? PGROUNDUP(p->sz) : 0;
  uint64_t covered_end = total;

  if (!p)
    return 0;
  for (struct vm_area *vma = p->vma_list; vma; vma = vma->next) {
    uint64_t start = vma->start;
    uint64_t end = vma->end;
    uint64_t add_start;
    if (end <= start || end <= covered_end)
      continue;
    add_start = start < covered_end ? covered_end : start;
    total += end - add_start;
    covered_end = end;
  }
  return total / PAGE_SIZE;
}

static uint64_t statm_text_pages(struct proc *p) {
  uint64_t pages = 0;
  if (!p)
    return 0;
  for (struct vm_area *vma = p->vma_list; vma; vma = vma->next) {
    if (vma->end > vma->start && (vma->prot & PROT_EXEC))
      pages += (vma->end - vma->start) / PAGE_SIZE;
  }
  return pages;
}

int procfs_render_statm(char *buf, int bufsz, struct proc *p) {
  uint64_t size = statm_vmsize_pages(p);
  uint64_t text = statm_text_pages(p);
  uint64_t data = size > text ? size - text : 0;
  int pos = 0;

  /* ENH: Keep Linux statm's seven-field shape.  Resident/shared are best
   * effort in this teaching kernel, so resident mirrors mapped pages and
   * shared/lib/dt stay zero until per-page residency accounting exists. */
  pos = append_int(buf, bufsz, pos, (long)size);
  pos = append_char(buf, bufsz, pos, ' ');
  pos = append_int(buf, bufsz, pos, (long)size);
  pos = append_char(buf, bufsz, pos, ' ');
  pos = append_int(buf, bufsz, pos, 0);
  pos = append_char(buf, bufsz, pos, ' ');
  pos = append_int(buf, bufsz, pos, (long)text);
  pos = append_char(buf, bufsz, pos, ' ');
  pos = append_int(buf, bufsz, pos, 0);
  pos = append_char(buf, bufsz, pos, ' ');
  pos = append_int(buf, bufsz, pos, (long)data);
  pos = append_char(buf, bufsz, pos, ' ');
  pos = append_int(buf, bufsz, pos, 0);
  pos = append_char(buf, bufsz, pos, '\n');
  return pos;
}

int procfs_render_comm(char *buf, int bufsz, struct proc *p) {
  int len = 0;
  const char *name = p ? p->name : "";

  while (name[len] && len < 15 && len < bufsz)
    len++;
  if (len > 0)
    memcpy(buf, name, len);
  if (len < bufsz)
    buf[len++] = '\n';
  return len;
}

int procfs_render_environ(char *buf, int bufsz, struct proc *p) {
  int len;

  if (!p || p->environ_len == 0)
    return 0;
  len = (int)p->environ_len;
  if (len > bufsz)
    len = bufsz;
  memcpy(buf, p->environ, len);
  return len;
}

int procfs_render_fdinfo(char *buf, int bufsz, struct proc *p, int fd) {
  struct open_file *f;
  uint64_t ino = 0;
  int pos = 0;

  if (!p || fd < 0 || fd >= NOFILE)
    return -ENOENT;
  f = proc_ofile(p)[fd];
  if (!f)
    return -ENOENT;

  if (!f->is_pipe && f->ops && f->ops->fstat) {
    uint8_t st[128];
    if (f->ops->fstat(f->fs_data, f->private, st) == 0)
      memcpy(&ino, st + 8, 8);
  } else if (f->is_pipe) {
    ino = (uint64_t)(f->pipe_id + 1);
  }

  pos = append_str(buf, bufsz, pos, "pos:\t");
  pos = append_int(buf, bufsz, pos, f->is_pipe ? 0 : (long)f->offset);
  pos = append_char(buf, bufsz, pos, '\n');
  pos = append_str(buf, bufsz, pos, "flags:\t");
  pos = append_octal(buf, bufsz, pos, (unsigned int)f->flags);
  pos = append_char(buf, bufsz, pos, '\n');
  pos = append_str(buf, bufsz, pos, "mnt_id:\t0\nino:\t");
  pos = append_int(buf, bufsz, pos, (long)ino);
  pos = append_char(buf, bufsz, pos, '\n');
  return pos;
}
