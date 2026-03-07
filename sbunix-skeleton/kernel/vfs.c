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
/* VFS init                                                             */
/* ------------------------------------------------------------------ */
void vfs_init(void) {
  memset(mount_table, 0, sizeof(mount_table));
  memset(file_table, 0, sizeof(file_table));
  num_mounts = 0;

  /* Register devfs at /dev first (longer prefix wins) */
  vfs_register_mount("/dev", &devfs_ops);
  /* Register tarfs at / (root) */
  vfs_register_mount("/", &tarfs_ops);

  printk("vfs: initialized (/dev -> devfs, / -> tarfs)\n");
}
