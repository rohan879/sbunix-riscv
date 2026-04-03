#include <ftw.h>

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int walk_rec(const char *path,
                    int (*fn)(const char *, const struct stat *, int, struct FTW *),
                    int flags,
                    int depth) {
  struct stat st;
  int st_rc = (flags & FTW_PHYS) ? lstat(path, &st) : stat(path, &st);
  if (st_rc < 0) {
    struct FTW info = { .base = 0, .level = depth };
    return fn(path, (const struct stat *)0, FTW_NS, &info);
  }

  const char *base = strrchr(path, '/');
  struct FTW info = {
    .base = base ? (int)(base - path + 1) : 0,
    .level = depth,
  };

  if (!S_ISDIR(st.st_mode))
    return fn(path, &st, FTW_F, &info);

  if (!(flags & FTW_DEPTH)) {
    int rc = fn(path, &st, FTW_D, &info);
    if (rc)
      return rc;
  }

  DIR *dp = opendir(path);
  if (!dp)
    return fn(path, &st, FTW_DNR, &info);

  int rc = 0;
  struct dirent *de;
  while ((de = readdir(dp)) != (void *)0) {
    if ((de->d_name[0] == '.' && de->d_name[1] == '\0') ||
        (de->d_name[0] == '.' && de->d_name[1] == '.' && de->d_name[2] == '\0'))
      continue;

    size_t plen = strlen(path);
    size_t nlen = strlen(de->d_name);
    int need_slash = plen > 0 && path[plen - 1] != '/';
    char *child = (char *)malloc(plen + need_slash + nlen + 1);
    if (!child) {
      rc = -1;
      break;
    }
    memcpy(child, path, plen);
    if (need_slash)
      child[plen++] = '/';
    memcpy(child + plen, de->d_name, nlen + 1);

    rc = walk_rec(child, fn, flags, depth + 1);
    free(child);
    if (rc)
      break;
  }
  closedir(dp);
  if (rc)
    return rc;

  if (flags & FTW_DEPTH)
    return fn(path, &st, FTW_DP, &info);
  return 0;
}

int nftw(const char *path,
         int (*fn)(const char *, const struct stat *, int, struct FTW *),
         int fd_limit,
         int flags) {
  (void)fd_limit;
  if (!path || !fn) {
    errno = EINVAL;
    return -1;
  }
  return walk_rec(path, fn, flags, 0);
}
