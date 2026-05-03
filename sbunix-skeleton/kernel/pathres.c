/* RF: Path resolution helpers extracted from trap.c.
 * Pure helper functions with no state -- used by syscall_io.c,
 * syscall_process.c (execve), syscall_misc.c (mount/utimensat),
 * and syscall_memory.c (file-backed mmap). */

#include <pathres.h>
#include <string.h>
#include <vfs.h>
#include <riscv.h>
#include <proc.h>
#include <printk.h>
#include <errno.h>
#include <syscall.h>
#include <vm.h>
#include <kmalloc.h>

/* Convert an absolute/relative path to the tarfs "./..." format.
 * tarfs uses paths like "./bin/echo", "./etc/rc", etc.
 * Used by execve to look up ELF binaries.
 * BF: Changed signature to accept outsize and return int (0 on success,
 * -36/ENAMETOOLONG on overflow) to prevent silent buffer overruns. */
int to_tarfs_path(const char *path, char *out, int outsize) {
    if (outsize < 2) return -36; /* ENAMETOOLONG */
    /* Strip leading slash(es) */
    while (*path == '/') path++;
    if (*path == '\0') {
        out[0] = '.'; out[1] = '\0';
        return 0;
    }
    out[0] = '.';
    out[1] = '/';
    int i = 2;
    int lim = outsize - 1; /* reserve null terminator */
    while (*path && i < lim) {
        out[i++] = *path++;
    }
    if (*path) return -36; /* ENAMETOOLONG — input too long */
    out[i] = '\0';
    return 0;
}

/* Normalize an absolute path in-place: collapse "." and ".." components.
 * Input must already be absolute (start with '/').  Result is written to out.
 * BF: Returns 0 on success, -36 (ENAMETOOLONG) if any component was
 * silently truncated due to buffer overflow. */
int normalize_path(const char *in, char *out) {
    /* Use a bounded stack of component offsets into out[] */
    /* We build the result into out directly, tracking segment starts. */
    char tmp[KPATH_MAX];
    int tlen = 0;
    const char *p = in;

    /* Skip leading slash — we'll add it back */
    while (*p == '/') p++;

    tmp[tlen++] = '/';
    /* Stack of segment-end positions in tmp (indices of the '/' before each segment) */
    int stack[(KPATH_MAX / 2) + 1];
    int depth = 0;

    while (*p) {
        /* Find end of this component */
        const char *start = p;
        while (*p && *p != '/') p++;
        int clen = (int)(p - start);

        if (clen == 0 || (clen == 1 && start[0] == '.')) {
            /* "." or empty — skip */
        } else if (clen == 2 && start[0] == '.' && start[1] == '.') {
            /* ".." — pop last segment */
            if (depth > 0) {
                tlen = stack[--depth];
            }
        } else {
            /* Normal component — push */
            if (depth >= (int)(sizeof(stack) / sizeof(stack[0])))
                return -ENAMETOOLONG;
            stack[depth++] = tlen; /* save position before adding '/' + component */
            if (tlen > 1) {
                if (tlen >= (int)sizeof(tmp) - 1)
                    return -ENAMETOOLONG;
                tmp[tlen++] = '/'; /* add separator (root already has '/') */
            }
            if (tlen + clen > (int)sizeof(tmp) - 1)
                return -ENAMETOOLONG;
            memcpy(tmp + tlen, start, clen);
            tlen += clen;
        }

        /* skip slashes between components */
        while (*p == '/') p++;
    }

    tmp[tlen] = '\0';
    /* Result: if only the root slash remains, that's "/" */
    memcpy(out, tmp, tlen + 1);

    return 0;
}

int copy_user_path_arg(uint64_t uaddr, char *dst, int dstsz, int allow_empty) {
    if (!dst || dstsz <= 0)
        return -EFAULT;

    /* BF: copyinstr returns -1 when a valid string does not fit in the
     * supplied buffer; that is ENAMETOOLONG, not EFAULT. */
    int ret = copyinstr(current_proc->pagetable, dst, uaddr, (uint64_t)dstsz);
    if (ret < 0)
        return ret == -1 ? -ENAMETOOLONG : -EFAULT;
    if (!allow_empty && dst[0] == '\0')
        return -ENOENT;
    return 0;
}

/* Resolve a path against cwd.  Result in out[KPATH_MAX].
 * BF: Empty paths are invalid for path lookup; callers that implement
 * AT_EMPTY_PATH handle that case before calling into the resolver. */
int resolve_path(const char *cwd, const char *path, char *out) {
    char raw[KPATH_MAX]; /* intermediate buffer before normalization */
    int rlen;

    if (path[0] == '\0') {
        out[0] = '\0';
        return -ENOENT;
    }

    if (path[0] == '/') {
        /* absolute */
        rlen = strlen(path);
        /* BF: Return -ENAMETOOLONG for absolute paths that exceed buffer,
         * instead of silently truncating (which resolves a different path). */
        if (rlen > (int)sizeof(raw) - 1) {
            out[0] = '/'; out[1] = '\0';
            return -ENAMETOOLONG;
        }
        memcpy(raw, path, rlen);
        raw[rlen] = '\0';
    } else {
        /* relative — prepend cwd */
        int cwdlen = strlen(cwd);
        int pathlen = strlen(path);
        int add_slash = (cwdlen == 0 || cwd[cwdlen - 1] != '/');
        /* BF: Use sizeof(raw)-2 instead of hardcoded 254 */
        if (cwdlen + add_slash + pathlen > (int)sizeof(raw) - 1) {
            /* BF: Return -ENAMETOOLONG instead of silently falling back to "/" */
            out[0] = '/'; out[1] = '\0';
            return -ENAMETOOLONG;
        }
        memcpy(raw, cwd, cwdlen);
        if (add_slash) {
            raw[cwdlen++] = '/';
        }
        memcpy(raw + cwdlen, path, pathlen);
        raw[cwdlen + pathlen] = '\0';
    }

    /* BF: Propagate ENAMETOOLONG from normalize_path */
    return normalize_path(raw, out);
}

/* BF: Centralize dirfd semantics for implemented *at syscalls so relative
 * paths consistently use AT_FDCWD or an open directory fd's saved path. */
int resolve_path_at(int dirfd, const char *path, char *out) {
    if (path[0] == '/' || dirfd == AT_FDCWD)
        return resolve_path(current_proc->cwd, path, out);

    struct open_file **ftab = proc_ofile(current_proc);
    if (dirfd < 0 || dirfd >= NOFILE || !ftab[dirfd])
        return -EBADF;

    struct open_file *dir = ftab[dirfd];
    if (dir->is_pipe || dir->path[0] == '\0' || !vfs_is_dir(dir->path))
        return -ENOTDIR;

    return resolve_path(dir->path, path, out);
}

/* Check if a resolved path exists as a directory by trying to open it */
int vfs_is_dir(const char *path) {
    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(path, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops) return 0;
    /* Try stat to check if it's a directory */
    uint8_t st[128];
    if (ops->stat(mnt->fs_data, relpath, st) == 0) {
        uint32_t mode;
        memcpy(&mode, st + 16, 4);
        return (mode & 0xF000) == 0x4000; /* S_IFDIR */
    }
    return 0;
}

/* Follow symlinks in a resolved path (up to 6 levels to prevent loops).
 * BF: Use heap scratch for target expansion so SYMLINK_MAX-sized targets
 * do not consume recursive kernel stack frames. */
int follow_symlinks(char *resolved, int maxdepth, int bufsiz) {
    if (maxdepth <= 0) return -40; /* ELOOP */

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->readlink) return 0; /* no readlink = not a symlink-capable fs */

    char *target = (char *)kmalloc(KSYMLINK_MAX + 1);
    if (!target)
        return -ENOMEM;

    int ret = ops->readlink(mnt->fs_data, relpath, target, KSYMLINK_MAX);
    if (ret == -EINVAL) {
        kmfree(target, KSYMLINK_MAX + 1);
        return 0; /* not a symlink, path is final */
    }
    if (ret < 0) {
        kmfree(target, KSYMLINK_MAX + 1);
        return ret;
    }
    target[ret] = '\0';

    char *new_resolved = (char *)kmalloc(KPATH_MAX);
    if (!new_resolved) {
        kmfree(target, KSYMLINK_MAX + 1);
        return -ENOMEM;
    }

    int rp_ret; /* BF: Propagate ENAMETOOLONG from resolve_path */
    if (target[0] == '/') {
        rp_ret = resolve_path("/", target, new_resolved);
        if (rp_ret < 0) {
            kmfree(new_resolved, KPATH_MAX);
            kmfree(target, KSYMLINK_MAX + 1);
            return rp_ret;
        }
    } else {
        /* Relative: resolve from symlink's parent directory */
        char *dir = (char *)kmalloc(KPATH_MAX);
        if (!dir) {
            kmfree(new_resolved, KPATH_MAX);
            kmfree(target, KSYMLINK_MAX + 1);
            return -ENOMEM;
        }
        int rlen = strlen(resolved);
        if (rlen > KPATH_MAX - 1) {
            kmfree(dir, KPATH_MAX);
            kmfree(new_resolved, KPATH_MAX);
            kmfree(target, KSYMLINK_MAX + 1);
            return -ENAMETOOLONG;
        }
        memcpy(dir, resolved, rlen);
        dir[rlen] = '\0';
        char *last_slash = dir;
        for (char *p = dir; *p; p++)
            if (*p == '/') last_slash = p;
        if (last_slash != dir)
            *(last_slash + 1) = '\0';
        else
            dir[1] = '\0';

        rp_ret = resolve_path(dir, target, new_resolved);
        kmfree(dir, KPATH_MAX);
        if (rp_ret < 0) {
            kmfree(new_resolved, KPATH_MAX);
            kmfree(target, KSYMLINK_MAX + 1);
            return rp_ret; /* BF: Propagate ENAMETOOLONG */
        }
    }

    int nlen = strlen(new_resolved);
    /* BF: Respect caller's buffer size instead of hardcoded 255 */
    if (nlen > bufsiz - 1) {
        kmfree(new_resolved, KPATH_MAX);
        kmfree(target, KSYMLINK_MAX + 1);
        return -ENAMETOOLONG;
    }
    memcpy(resolved, new_resolved, nlen);
    resolved[nlen] = '\0';

    kmfree(new_resolved, KPATH_MAX);
    kmfree(target, KSYMLINK_MAX + 1);

    /* Recursively follow in case target is also a symlink */
    return follow_symlinks(resolved, maxdepth - 1, bufsiz);
}

/* Walk a resolved path component-by-component, resolving symlinks at
 * every intermediate directory.  The final component is also resolved
 * unless resolve_final is 0.  After return, 'resolved' contains the
 * fully-resolved path.
 * Returns 0 on success, negative errno on failure (-40 = ELOOP). */
int resolve_symlinks_in_path(char *resolved, int maxdepth, int resolve_final) {
    if (maxdepth <= 0) return -40; /* ELOOP */

    int rlen = strlen(resolved);
    if (rlen == 0 || (rlen == 1 && resolved[0] == '/'))
        return 0; /* root or empty — nothing to resolve */
    if (rlen > KPATH_MAX - 1)
        return -ENAMETOOLONG;

    /* Split into components */
    char *work = (char *)kmalloc(KPATH_MAX);
    char *built = (char *)kmalloc(KPATH_MAX);
    char *before = (char *)kmalloc(KPATH_MAX);
    char *normalized = (char *)kmalloc(KPATH_MAX);
    if (!work || !built || !before || !normalized) {
        if (work) kmfree(work, KPATH_MAX);
        if (built) kmfree(built, KPATH_MAX);
        if (before) kmfree(before, KPATH_MAX);
        if (normalized) kmfree(normalized, KPATH_MAX);
        return -ENOMEM;
    }
    int ret = 0;
    memcpy(work, resolved, rlen);
    work[rlen] = '\0';

    int bi = 0;
    int bmax = KPATH_MAX - 1; /* max usable index */

    int wi = 0; /* index into work */

    /* Handle leading slash */
    if (work[0] == '/') {
        built[bi++] = '/';
        wi = 1;
    }
    built[bi] = '\0';

    while (wi < rlen) {
        /* Skip consecutive slashes */
        while (wi < rlen && work[wi] == '/') wi++;
        if (wi >= rlen) break;

        /* Extract next component */
        int comp_start = wi;
        while (wi < rlen && work[wi] != '/') wi++;
        int comp_len = wi - comp_start;

        /* Check if there are more components after this one */
        int tmp_wi = wi;
        while (tmp_wi < rlen && work[tmp_wi] == '/') tmp_wi++;
        int is_last = (tmp_wi >= rlen);

        /* Append separator if needed */
        if (bi > 0 && built[bi - 1] != '/') {
            if (bi < bmax) built[bi++] = '/';
        }
        if (bi + comp_len > bmax)
            { ret = -ENAMETOOLONG; goto out; }
        memcpy(built + bi, work + comp_start, comp_len);
        bi += comp_len;
        built[bi] = '\0';

        /* Skip symlink resolution for the final component when
         * the caller asked us not to resolve it (O_NOFOLLOW, etc.) */
        if (is_last && !resolve_final)
            break;

        /* Try to resolve this component as a symlink */
        int old_bi = bi;
        memcpy(before, built, bi + 1);

        int sym_ret = follow_symlinks(built, maxdepth, KPATH_MAX);
        /* BF: Propagate all negative errors, not just ELOOP */
        if (sym_ret < 0) { ret = sym_ret; goto out; }

        bi = strlen(built);

        /* Check if follow_symlinks changed the path */
        int changed = (bi != old_bi || memcmp(before, built, old_bi + 1) != 0);

        /* If this was an intermediate component and it resolved to a
         * different path, append the remaining components and recurse. */
        if (!is_last && changed) {
            /* Symlink was resolved; append remaining path */
            int remaining = rlen - wi;
            if (remaining > 0) {
                if (bi > 0 && built[bi - 1] != '/' && work[wi] != '/') {
                    if (bi < bmax) built[bi++] = '/';
                }
                /* BF: Return ENAMETOOLONG instead of silently truncating the
                 * remaining suffix, which would resolve a different path. */
                if (bi + remaining > bmax) { ret = -ENAMETOOLONG; goto out; }
                memcpy(built + bi, work + wi, remaining);
                bi += remaining;
                built[bi] = '\0';

                /* BF: Propagate ENAMETOOLONG from resolve_path */
                int rp_ret = resolve_path("/", built, normalized);
                if (rp_ret < 0) { ret = rp_ret; goto out; }
                int nlen = strlen(normalized);
                /* BF: Return ENAMETOOLONG instead of silently truncating
                 * the normalized path, which could resolve wrong target. */
                if (nlen >= KPATH_MAX) { ret = -ENAMETOOLONG; goto out; }
                memcpy(resolved, normalized, nlen);
                resolved[nlen] = '\0';

                /* Recurse to resolve any further symlinks */
                ret = resolve_symlinks_in_path(resolved, maxdepth - 1, resolve_final);
                goto out;
            }
        }
    }

    /* Copy result back */
    /* BF: Return ENAMETOOLONG instead of silently clamping the built
     * path length, which would produce a truncated (wrong) path. */
    if (bi > bmax) { ret = -ENAMETOOLONG; goto out; }
    memcpy(resolved, built, bi);
    resolved[bi] = '\0';

out:
    kmfree(normalized, KPATH_MAX);
    kmfree(before, KPATH_MAX);
    kmfree(built, KPATH_MAX);
    kmfree(work, KPATH_MAX);
    return ret;
}
