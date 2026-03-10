/*
 * pwd.c — getpwuid, getpwnam, getgrgid, getgrnam stubs for SBUnix libc.
 *
 * Returns static root user/group entries.
 */

#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stddef.h>

static char root_name[]   = "root";
static char root_passwd[] = "x";
static char root_dir[]    = "/";
static char root_shell[]  = "/bin/sh";
static char *root_mem[]   = { root_name, (char *)0 };

static struct passwd root_pw = {
    root_name, root_passwd, 0, 0, root_name, root_dir, root_shell
};

static struct group root_gr = {
    root_name, root_passwd, 0, root_mem
};

struct passwd *getpwuid(uid_t uid) {
    if (uid == 0) return &root_pw;
    return (void *)0;
}

struct passwd *getpwnam(const char *name) {
    if (name && strcmp(name, "root") == 0) return &root_pw;
    return (void *)0;
}

struct group *getgrgid(gid_t gid) {
    if (gid == 0) return &root_gr;
    return (void *)0;
}

struct group *getgrnam(const char *name) {
    if (name && strcmp(name, "root") == 0) return &root_gr;
    return (void *)0;
}
