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

/* Convert an absolute/relative path to the tarfs "./..." format.
 * tarfs uses paths like "./bin/echo", "./etc/rc", etc.
 * Used by execve to look up ELF binaries. */
void to_tarfs_path(const char *path, char *out) {
    /* Strip leading slash(es) */
    while (*path == '/') path++;
    if (*path == '\0') {
        out[0] = '.'; out[1] = '\0';
        return;
    }
    out[0] = '.';
    out[1] = '/';
    int i = 2;
    while (*path && i < 255) {
        out[i++] = *path++;
    }
    out[i] = '\0';
}

/* Normalize an absolute path in-place: collapse "." and ".." components.
 * Input must already be absolute (start with '/').  Result is written to out.
 * BF: Returns 0 on success, -36 (ENAMETOOLONG) if any component was
 * silently truncated due to buffer overflow. */
int normalize_path(const char *in, char *out) {
    /* Use a small stack of component offsets into out[] */
    /* We build the result into out directly, tracking segment starts. */
    char tmp[128];
    int tlen = 0;
    const char *p = in;
    /* BF: Track whether any component was dropped due to truncation */
    int truncated = 0;

    /* Skip leading slash — we'll add it back */
    while (*p == '/') p++;

    tmp[tlen++] = '/';
    /* Stack of segment-end positions in tmp (indices of the '/' before each segment) */
    int stack[64];
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
            stack[depth++] = tlen; /* save position before adding '/' + component */
            if (tlen > 1) tmp[tlen++] = '/'; /* add separator (root already has '/') */
            /* BF: Use sizeof(tmp) instead of hardcoded 255 to prevent
             * buffer overflow — tmp is only 128 bytes, not 256. */
            if (tlen + clen < (int)sizeof(tmp) - 1) {
                memcpy(tmp + tlen, start, clen);
                tlen += clen;
            } else {
                /* BF: Component dropped — flag truncation for ENAMETOOLONG */
                truncated = 1;
            }
        }

        /* skip slashes between components */
        while (*p == '/') p++;
    }

    tmp[tlen] = '\0';
    /* Result: if only the root slash remains, that's "/" */
    memcpy(out, tmp, tlen + 1);

    /* BF: Return -ENAMETOOLONG if truncation occurred */
    return truncated ? -36 : 0;
}

/* Resolve a path against cwd.  Result in out[128].
 * BF: Returns 0 on success, -36 (ENAMETOOLONG) when the combined
 * path exceeds the internal buffer or normalize_path truncates. */
/* BF: Reduced raw[] from 256 to 128 to prevent kernel stack overflow.
 * Deep call chains (openat -> resolve_path -> resolve_symlinks ->
 * ext2_open -> bcache -> virtio_blk) on 4KB stacks caused corruption. */
int resolve_path(const char *cwd, const char *path, char *out) {
    char raw[128]; /* intermediate buffer before normalization */
    int rlen;

    if (path[0] == '/') {
        /* absolute */
        rlen = strlen(path);
        /* BF: Cap at sizeof(raw)-1 instead of 255 — raw is 128 bytes */
        if (rlen > (int)sizeof(raw) - 1) rlen = (int)sizeof(raw) - 1;
        memcpy(raw, path, rlen);
        raw[rlen] = '\0';
    } else {
        /* relative — prepend cwd */
        int cwdlen = strlen(cwd);
        int pathlen = strlen(path);
        /* BF: Use sizeof(raw)-2 instead of hardcoded 254 */
        if (cwdlen + 1 + pathlen > (int)sizeof(raw) - 2) {
            /* BF: Return -ENAMETOOLONG instead of silently falling back to "/" */
            out[0] = '/'; out[1] = '\0';
            return -36; /* -ENAMETOOLONG */
        }
        memcpy(raw, cwd, cwdlen);
        if (cwdlen == 0 || cwd[cwdlen-1] != '/') {
            raw[cwdlen++] = '/';
        }
        memcpy(raw + cwdlen, path, pathlen);
        raw[cwdlen + pathlen] = '\0';
    }

    /* BF: Propagate ENAMETOOLONG from normalize_path */
    return normalize_path(raw, out);
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
 * BF: Added bufsiz parameter so the copy-back step respects the caller's
 * actual buffer size instead of assuming 256 bytes.
 * BF: Reduced from 8 to 6 levels; each frame uses ~640 bytes of stack
 * (target[128] + new_resolved[128] + dir[128] + combined[128] + locals). */
int follow_symlinks(char *resolved, int maxdepth, int bufsiz) {
    if (maxdepth <= 0) return -40; /* ELOOP */

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->readlink) return 0; /* no readlink = not a symlink-capable fs */

    /* BF: Increased from 64 to 128 to match ramfs symlink_target size
     * and prevent truncation of longer symlink targets. */
    char target[128];
    int ret = ops->readlink(mnt->fs_data, relpath, target, sizeof(target) - 1);
    if (ret < 0) return 0; /* not a symlink, path is final */
    target[ret] = '\0';

    /* Resolve the symlink target.  Use small local buffers to avoid
     * kernel stack overflow — follow_symlinks recurses up to 6 levels
     * and each frame consumes the sum of all local arrays. */
    char new_resolved[128];
    int rp_ret; /* BF: Propagate ENAMETOOLONG from resolve_path */
    if (target[0] == '/') {
        rp_ret = resolve_path("/", target, new_resolved);
        if (rp_ret < 0) return rp_ret;
    } else {
        /* Relative: resolve from symlink's parent directory */
        char dir[128];
        int rlen = strlen(resolved);
        if (rlen > (int)sizeof(dir) - 1) rlen = (int)sizeof(dir) - 1;
        memcpy(dir, resolved, rlen);
        dir[rlen] = '\0';
        char *last_slash = dir;
        for (char *p = dir; *p; p++)
            if (*p == '/') last_slash = p;
        if (last_slash != dir)
            *(last_slash + 1) = '\0';
        else
            dir[1] = '\0';

        char combined[128];
        int dlen = strlen(dir);
        int tlen = strlen(target);
        if (dlen + tlen >= (int)sizeof(combined) - 2) return -36; /* ENAMETOOLONG */
        memcpy(combined, dir, dlen);
        memcpy(combined + dlen, target, tlen + 1);

        rp_ret = resolve_path("/", combined, new_resolved);
        if (rp_ret < 0) return rp_ret; /* BF: Propagate ENAMETOOLONG */
    }

    int nlen = strlen(new_resolved);
    /* BF: Respect caller's buffer size instead of hardcoded 255 */
    if (nlen > bufsiz - 1) nlen = bufsiz - 1;
    memcpy(resolved, new_resolved, nlen);
    resolved[nlen] = '\0';

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

    /* Split into components */
    char work[128];
    /* BF: Use sizeof(work)-1 instead of hardcoded 255 to match actual
     * buffer size and prevent out-of-bounds writes. */
    if (rlen > (int)sizeof(work) - 1) rlen = (int)sizeof(work) - 1;
    memcpy(work, resolved, rlen);
    work[rlen] = '\0';

    char built[128]; /* path built so far */
    int bi = 0;
    int bmax = (int)sizeof(built) - 1; /* max usable index */

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
            return -36; /* ENAMETOOLONG */
        memcpy(built + bi, work + comp_start, comp_len);
        bi += comp_len;
        built[bi] = '\0';

        /* Skip symlink resolution for the final component when
         * the caller asked us not to resolve it (O_NOFOLLOW, etc.) */
        if (is_last && !resolve_final)
            break;

        /* Try to resolve this component as a symlink */
        int old_bi = bi;
        char before[128];
        memcpy(before, built, bi + 1);

        int sym_ret = follow_symlinks(built, maxdepth, sizeof(built));
        /* BF: Propagate all negative errors, not just ELOOP */
        if (sym_ret < 0) return sym_ret;

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
                if (bi + remaining > bmax) remaining = bmax - bi;
                memcpy(built + bi, work + wi, remaining);
                bi += remaining;
                built[bi] = '\0';

                /* BF: Propagate ENAMETOOLONG from resolve_path */
                char normalized[128];
                int rp_ret = resolve_path("/", built, normalized);
                if (rp_ret < 0) return rp_ret;
                int nlen = strlen(normalized);
                if (nlen > bmax) nlen = bmax;
                memcpy(resolved, normalized, nlen);
                resolved[nlen] = '\0';

                /* Recurse to resolve any further symlinks */
                return resolve_symlinks_in_path(resolved, maxdepth - 1, resolve_final);
            }
        }
    }

    /* Copy result back */
    if (bi > bmax) bi = bmax;
    memcpy(resolved, built, bi);
    resolved[bi] = '\0';
    return 0;
}
