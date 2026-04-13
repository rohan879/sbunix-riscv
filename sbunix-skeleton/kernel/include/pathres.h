/* RF: Path resolution interface for syscall modules.
 * Part of the trap.c modular decomposition -- implementation in
 * kernel/pathres.c, used by syscall_io.c, syscall_process.c,
 * syscall_misc.c, and syscall_memory.c. */
#ifndef _PATHRES_H
#define _PATHRES_H

void to_tarfs_path(const char *path, char *out);
/* BF: Changed return type from void to int to propagate ENAMETOOLONG */
int  normalize_path(const char *in, char *out);
int  resolve_path(const char *cwd, const char *path, char *out);
int  vfs_is_dir(const char *path);
int  follow_symlinks(char *resolved, int maxdepth, int bufsiz);
int  resolve_symlinks_in_path(char *resolved, int maxdepth, int resolve_final);

#endif
