/*
 * pwd.c — getpwuid, getpwnam, getgrgid, getgrnam stubs for SBUnix libc.
 *
 * Returns a tiny static passwd/group database for Open POSIX permission tests.
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
static char nobody_name[]   = "nobody";
static char nobody_passwd[] = "x";
static char nobody_dir[]    = "/";
static char nobody_shell[]  = "/bin/sh";
static char *nobody_mem[]   = { nobody_name, (char *)0 };

static struct passwd root_pw = {
    root_name, root_passwd, 0, 0, root_name, root_dir, root_shell
};
static struct passwd nobody_pw = {
    nobody_name, nobody_passwd, 65534, 65534, nobody_name, nobody_dir,
    nobody_shell
};

static struct group root_gr = {
    root_name, root_passwd, 0, root_mem
};
static struct group nobody_gr = {
    nobody_name, nobody_passwd, 65534, nobody_mem
};

static int pw_iter_index;

struct passwd *getpwuid(uid_t uid) {
    if (uid == 0) return &root_pw;
    if (uid == 65534) return &nobody_pw;
    return (void *)0;
}

struct passwd *getpwnam(const char *name) {
    if (name && strcmp(name, "root") == 0) return &root_pw;
    if (name && strcmp(name, "nobody") == 0) return &nobody_pw;
    return (void *)0;
}

struct passwd *getpwent(void) {
    if (pw_iter_index == 0) {
        pw_iter_index = 1;
        return &root_pw;
    }
    if (pw_iter_index == 1) {
        pw_iter_index = 2;
        return &nobody_pw;
    }
    return (void *)0;
}

void setpwent(void) {
    pw_iter_index = 0;
}

void endpwent(void) {
    pw_iter_index = 2;
}

struct group *getgrgid(gid_t gid) {
    if (gid == 0) return &root_gr;
    if (gid == 65534) return &nobody_gr;
    return (void *)0;
}

struct group *getgrnam(const char *name) {
    if (name && strcmp(name, "root") == 0) return &root_gr;
    if (name && strcmp(name, "nobody") == 0) return &nobody_gr;
    return (void *)0;
}
