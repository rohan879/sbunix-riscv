/* RF: Path resolution interface for syscall modules.
 * Part of the trap.c modular decomposition -- implementation in
 * kernel/pathres.c, used by syscall_io.c, syscall_process.c,
 * syscall_misc.c, and syscall_memory.c. */
#ifndef _PATHRES_H
#define _PATHRES_H

#include <riscv.h>

/* Paths stored in proc->cwd and open_file->path are 256-byte snapshots.
 * Keep resolver outputs capped to that size and report ENAMETOOLONG rather
 * than truncating to a different path. */
#define KPATH_MAX     256
#define KSYMLINK_MAX  255
#define KSYMLINK_MAX_HOPS 40

/* BF: Changed signature to accept outsize and return error on overflow */
int  to_tarfs_path(const char *path, char *out, int outsize);
/* BF: Changed return type from void to int to propagate ENAMETOOLONG */
int  normalize_path(const char *in, char *out);
int  copy_user_path_arg(uint64_t uaddr, char *dst, int dstsz, int allow_empty);
int  path_has_trailing_slash(const char *path);
int  path_stat_mode(const char *path, uint32_t *mode_out);
int  path_require_dir(const char *path);
int  path_resolve_trailing_slash_dir_target(char *resolved,
                                            int allow_missing_leaf);
int  resolve_path(const char *cwd, const char *path, char *out);
int  resolve_path_at(int dirfd, const char *path, char *out);
int  vfs_is_dir(const char *path);
int  follow_symlinks(char *resolved, int maxdepth, int bufsiz);

struct path_resolution_meta {
  int final_symlink_followed;
  int missing_after_final_symlink;
};

int  resolve_symlinks_in_path_meta(char *resolved, int maxdepth,
                                   int resolve_final,
                                   struct path_resolution_meta *meta);
int  resolve_symlinks_in_path(char *resolved, int maxdepth, int resolve_final);

#endif
