#include <vfs.h>
#include <string.h>
#include <printk.h>

/* ------------------------------------------------------------------ */
/* Mount table                                                          */
/* ------------------------------------------------------------------ */
#define MAX_MOUNTS 8
static struct mount mount_table[MAX_MOUNTS];
static int num_mounts = 0;

void vfs_register_mount(const char *path, struct vfs_ops *ops) {
  if (num_mounts >= MAX_MOUNTS) {
    printk("vfs: mount table full\n");
    return;
  }
  mount_table[num_mounts].path = path;
  mount_table[num_mounts].path_len = strlen(path);
  mount_table[num_mounts].ops = ops;
  num_mounts++;
}

/* Longest-prefix match on path.  Sets *relpath to the remainder. */
struct vfs_ops *vfs_lookup_mount(const char *path, const char **relpath) {
  struct mount *best = NULL;
  int best_len = -1;

  for (int i = 0; i < num_mounts; i++) {
    struct mount *m = &mount_table[i];
    int mlen = m->path_len;

    /* Check if path starts with this mount prefix */
    if (mlen == 1 && m->path[0] == '/') {
      /* Root mount matches everything */
      if (best_len < 1) {
        best = m;
        best_len = 1;
      }
    } else if (memcmp(path, m->path, mlen) == 0 &&
               (path[mlen] == '/' || path[mlen] == '\0')) {
      if (mlen > best_len) {
        best = m;
        best_len = mlen;
      }
    }
  }

  if (!best) {
    if (relpath) *relpath = path;
    return NULL;
  }

  if (relpath) {
    if (best_len == 1 && best->path[0] == '/') {
      /* Root mount: relpath is the full path */
      *relpath = path;
    } else {
      /* Strip the mount prefix */
      const char *rest = path + best_len;
      if (*rest == '/') rest++;
      if (*rest == '\0') {
        *relpath = "/";
      } else {
        *relpath = rest;
      }
    }
  }

  return best->ops;
}

/* ------------------------------------------------------------------ */
/* Open file pool                                                       */
/* ------------------------------------------------------------------ */
static struct open_file file_table[NFILE];

struct open_file *vfs_alloc_file(void) {
  for (int i = 0; i < NFILE; i++) {
    if (file_table[i].refcount == 0) {
      memset(&file_table[i], 0, sizeof(struct open_file));
      file_table[i].refcount = 1;
      return &file_table[i];
    }
  }
  return NULL;
}

void vfs_free_file(struct open_file *f) {
  if (!f) return;
  f->refcount--;
  if (f->refcount <= 0) {
    memset(f, 0, sizeof(struct open_file));
  }
}

/* ------------------------------------------------------------------ */
/* vfs_get_child_mounts — return basenames of mounts under dir         */
/* ------------------------------------------------------------------ */
int vfs_get_child_mounts(const char *dir, const char *names[], int max) {
  int dlen = strlen(dir);
  int count = 0;

  for (int i = 0; i < num_mounts && count < max; i++) {
    struct mount *m = &mount_table[i];
    /* Skip root mount itself */
    if (m->path_len <= 1) continue;

    if (dlen == 1 && dir[0] == '/') {
      /* Listing root: child mounts are those with path "/<name>" (no further /) */
      if (m->path[0] == '/') {
        const char *rest = m->path + 1;
        /* Only direct children — no slash in rest */
        int has_slash = 0;
        for (const char *c = rest; *c; c++) {
          if (*c == '/') { has_slash = 1; break; }
        }
        if (!has_slash && *rest != '\0')
          names[count++] = rest;
      }
    } else {
      /* Listing non-root dir: child mounts start with "dir/" */
      if (m->path_len > dlen &&
          memcmp(m->path, dir, dlen) == 0 &&
          m->path[dlen] == '/') {
        const char *rest = m->path + dlen + 1;
        int has_slash = 0;
        for (const char *c = rest; *c; c++) {
          if (*c == '/') { has_slash = 1; break; }
        }
        if (!has_slash && *rest != '\0')
          names[count++] = rest;
      }
    }
  }
  return count;
}

/* ------------------------------------------------------------------ */
/* VFS init                                                             */
/* ------------------------------------------------------------------ */
void vfs_init(void) {
  memset(mount_table, 0, sizeof(mount_table));
  memset(file_table, 0, sizeof(file_table));
  num_mounts = 0;

  /* Register devfs at /dev first (longer prefix wins) */
  vfs_register_mount("/dev", &devfs_ops);
  /* Register procfs at /proc */
  vfs_register_mount("/proc", &procfs_ops);
  /* Register ext2 at /disk (if mounted) */
  extern int ext2_mounted_flag(void);
  if (ext2_mounted_flag())
    vfs_register_mount("/disk", &ext2_ops);
  /* Register tarfs at / (root) */
  vfs_register_mount("/", &tarfs_ops);

  printk("vfs: initialized (/dev -> devfs, /proc -> procfs, / -> tarfs)\n");
}
