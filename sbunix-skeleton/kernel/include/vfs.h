#ifndef _VFS_H
#define _VFS_H

#include <riscv.h>

/* Forward declarations */
struct stat;
struct dirent;

struct vfs_ops {
  int (*open)(void *fs_data, const char *path, int flags, void **private);
  int64_t (*read)(void *fs_data, void *private, char *buf, uint64_t count,
                  int64_t *offset);
  int64_t (*write)(void *fs_data, void *private, const char *buf,
                   uint64_t count, int64_t *offset);
  int (*ftruncate)(void *fs_data, void *private, uint64_t length);
  int (*close)(void *fs_data, void *private);
  int (*stat)(void *fs_data, const char *path, uint8_t *st);
  int (*fstat)(void *fs_data, void *private, uint8_t *st);  /* per-fd stat */
  int (*readdir)(void *fs_data, void *private, char *buf, unsigned int count,
                 int64_t *offset);
  int (*mkdir)(void *fs_data, const char *path, int mode);
  int (*unlink)(void *fs_data, const char *path);
  int (*rmdir)(void *fs_data, const char *path);
  int (*symlink)(void *fs_data, const char *target, const char *linkpath);
  int (*readlink)(void *fs_data, const char *path, char *buf, int bufsiz);
  int (*rename)(void *fs_data, const char *oldpath, const char *newpath);
  /* ENH: Hard link support for linkat(2).  Creates a new directory entry
   * (newpath) that refers to the same inode as oldpath. */
  int (*link)(void *fs_data, const char *oldpath, const char *newpath);
};

/* Open file description -- stored in proc->ofile[] */
struct open_file {
  struct vfs_ops *ops;
  void *fs_data;
  void *private;
  int64_t offset;
  int flags;
  int refcount;   /* for dup/fork sharing */
  /* pipe fields -- pipes bypass VFS */
  int is_pipe;
  int pipe_read;
  int pipe_write;
  int pipe_id;
  /* resolved path for mount-point injection in readdir */
  char path[256];
};

/* Filesystem mount point */
struct mount {
  const char *path;     /* mount path prefix (e.g., "/", "/dev") */
  int path_len;
  struct vfs_ops *ops;
  void *fs_data;
  const char *fstype;
};

/* Maximum open file descriptions system-wide */
#define NFILE 512

/* VFS functions */
struct open_file *vfs_alloc_file(void);
void vfs_free_file(struct open_file *f);
void vfs_close_file(struct open_file *f);
int vfs_file_valid(struct open_file *f);
struct vfs_ops *vfs_lookup_mount(const char *path, const char **relpath);
struct mount *vfs_lookup_mount_entry(const char *path, const char **relpath);
void vfs_register_mount(const char *path, struct vfs_ops *ops, void *fs_data,
                        const char *fstype);
int vfs_unregister_mount(const char *path);
void vfs_init(void);
int vfs_get_child_mounts(const char *dir, const char *names[], int max);
int vfs_get_mount_info(int index, const char **path, const char **fstype);

/* Filesystem ops (defined in their respective .c files) */
extern struct vfs_ops tarfs_ops;
extern struct vfs_ops devfs_ops;
extern struct vfs_ops ext2_ops;
extern struct vfs_ops procfs_ops;
extern struct vfs_ops sysfs_ops;
extern struct vfs_ops ramfs_ops;

/* ramfs init (creates in-memory root inode) */
void ramfs_init(void);

/* devfs helpers */
int devfs_is_tty(void *private);

/* File lock entry -- used for both POSIX (per-PID) and OFD (per-open_file)
 * record locks.  Lock matching uses path string comparison because
 * struct open_file has no inode number field. */
#define MAX_FILE_LOCKS 256

struct file_lock {
  int used;                /* 1 if slot is in use */
  short l_type;            /* F_RDLCK(0), F_WRLCK(1) */
  int64_t l_start;         /* absolute start offset */
  int64_t l_len;           /* length (0 = to EOF) */
  pid_t l_pid;             /* owning PID (POSIX locks) or -1 (OFD) */
  struct open_file *ofd;   /* owning open_file ptr (OFD) or NULL (POSIX) */
  char path[256];          /* file path for lock matching */
};

extern struct file_lock lock_table[MAX_FILE_LOCKS];

/* Set or release a file lock.  Returns 0 on success, -EAGAIN on conflict. */
int do_file_setlk(const char *path, short type, int64_t start, int64_t len,
                   pid_t pid, struct open_file *ofd, int is_ofd, int wait);
/* Query whether a hypothetical lock would conflict.  Fills out_* with the
 * conflicting lock info, or sets *out_type = F_UNLCK(2) if no conflict. */
int do_file_getlk(const char *path, short type, int64_t start, int64_t len,
                   pid_t pid, struct open_file *ofd, int is_ofd,
                   short *out_type, int64_t *out_start, int64_t *out_len,
                   pid_t *out_pid);
/* Release all POSIX locks owned by pid (called on process exit). */
void file_locks_release_pid(pid_t pid);
/* Release all OFD locks owned by the given open_file (called on close). */
void file_locks_release_ofd(struct open_file *ofd);

#endif
