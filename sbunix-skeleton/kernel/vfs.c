#include <vfs.h>
#include <string.h>
#include <printk.h>

/* ------------------------------------------------------------------ */
/* Mount table                                                          */
/* ------------------------------------------------------------------ */
#define MAX_MOUNTS 8
static struct mount mount_table[MAX_MOUNTS];
static int num_mounts = 0;

void vfs_register_mount(const char *path, struct vfs_ops *ops, void *fs_data,
                        const char *fstype) {
  if (num_mounts >= MAX_MOUNTS) {
    printk("vfs: mount table full\n");
    return;
  }
  mount_table[num_mounts].path = path;
  mount_table[num_mounts].path_len = strlen(path);
  mount_table[num_mounts].ops = ops;
  mount_table[num_mounts].fs_data = fs_data;
  mount_table[num_mounts].fstype = fstype;
  num_mounts++;
}

int vfs_unregister_mount(const char *path) {
  for (int i = 0; i < num_mounts; i++) {
    if (strcmp(mount_table[i].path, path) == 0) {
      for (int j = i; j < num_mounts - 1; j++)
        mount_table[j] = mount_table[j + 1];
      num_mounts--;
      return 0;
    }
  }
  return -1;
}

/* Longest-prefix match on path.  Sets *relpath to the remainder. */
struct mount *vfs_lookup_mount_entry(const char *path, const char **relpath) {
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

  return best;
}

struct vfs_ops *vfs_lookup_mount(const char *path, const char **relpath) {
  struct mount *m = vfs_lookup_mount_entry(path, relpath);
  return m ? m->ops : NULL;
}

/* ------------------------------------------------------------------ */
/* Open file pool                                                       */
/* ------------------------------------------------------------------ */
static struct open_file file_table[NFILE];
static int vfs_bad_free_warnings;

static int vfs_file_ptr_valid(struct open_file *f) {
  uint64_t p = (uint64_t)f;
  uint64_t base = (uint64_t)&file_table[0];
  uint64_t end = (uint64_t)&file_table[NFILE];
  if (p < base || p >= end)
    return 0;
  if (((p - base) % (uint64_t)sizeof(struct open_file)) != 0)
    return 0;
  return 1;
}

int vfs_file_valid(struct open_file *f) {
  return vfs_file_ptr_valid(f);
}

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
  if (!vfs_file_ptr_valid(f)) {
    if (vfs_bad_free_warnings < 16) {
      printk("vfs: invalid file free 0x%lx ignored\n", (uint64_t)f);
      vfs_bad_free_warnings++;
    }
    return;
  }
  f->refcount--;
  if (f->refcount <= 0) {
    memset(f, 0, sizeof(struct open_file));
  }
}

void vfs_close_file(struct open_file *f) {
  if (!f) return;
  if (!vfs_file_ptr_valid(f)) {
    if (vfs_bad_free_warnings < 16) {
      printk("vfs: invalid file close 0x%lx ignored\n", (uint64_t)f);
      vfs_bad_free_warnings++;
    }
    return;
  }

  f->refcount--;
  if (f->refcount <= 0) {
    /* BF: Centralize final open-file teardown so fd close and VMA release
     * both run filesystem close callbacks and release OFD locks exactly
     * when the last reference is dropped. */
    file_locks_release_ofd(f);
    if (!f->is_pipe && f->ops && f->ops->close)
      f->ops->close(f->fs_data, f->private);
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
/* vfs_get_mount_info — return mount path and fs type by index          */
/* ------------------------------------------------------------------ */
int vfs_get_mount_info(int index, const char **path, const char **fstype) {
  if (index < 0 || index >= num_mounts)
    return -1;
  struct mount *m = &mount_table[index];
  if (path) *path = m->path;
  if (fstype) {
    if (m->fstype)                  *fstype = m->fstype;
    else if (m->ops == &tarfs_ops)  *fstype = "tarfs";
    else if (m->ops == &devfs_ops)  *fstype = "devfs";
    else if (m->ops == &procfs_ops) *fstype = "procfs";
    else if (m->ops == &ext2_ops)   *fstype = "ext2";
    else if (m->ops == &sysfs_ops)  *fstype = "sysfs";
    else if (m->ops == &ramfs_ops)  *fstype = "ramfs";
    else                            *fstype = "unknown";
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* VFS init                                                             */
/* ------------------------------------------------------------------ */
void vfs_init(void) {
  memset(mount_table, 0, sizeof(mount_table));
  memset(file_table, 0, sizeof(file_table));
  num_mounts = 0;

  /* Register devfs at /dev first (longer prefix wins) */
  vfs_register_mount("/dev", &devfs_ops, NULL, "devfs");
  /* Register procfs at /proc */
  vfs_register_mount("/proc", &procfs_ops, NULL, "procfs");
  /* Register sysfs at /sys */
  vfs_register_mount("/sys", &sysfs_ops, NULL, "sysfs");
  /* Register ext2 instances discovered at boot. */
  extern int ext2_register_mounts(void);
  ext2_register_mounts();
  /* Register ramfs at /tmp (writable in-memory filesystem) */
  ramfs_init();
  vfs_register_mount("/tmp", &ramfs_ops, NULL, "ramfs");
  /* Register tarfs at / (root) */
  vfs_register_mount("/", &tarfs_ops, NULL, "tarfs");

  printk("vfs: initialized (/dev -> devfs, /proc -> procfs, /sys -> sysfs, /tmp -> ramfs, / -> tarfs)\n");
}
