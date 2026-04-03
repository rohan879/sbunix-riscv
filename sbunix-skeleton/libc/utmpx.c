#include <utmpx.h>
#include <string.h>
#include <errno.h>

static struct utmpx entries[8];
static int entries_init;
static size_t cursor;

static void init_entries(void) {
  if (entries_init)
    return;
  memset(entries, 0, sizeof(entries));
  entries[0].ut_type = BOOT_TIME;
  entries[1].ut_type = USER_PROCESS;
  entries[1].ut_pid = 1;
  strncpy(entries[1].ut_line, "tty0", sizeof(entries[1].ut_line) - 1);
  strncpy(entries[1].ut_user, "root", sizeof(entries[1].ut_user) - 1);
  entries_init = 1;
}

void setutxent(void) {
  init_entries();
  cursor = 0;
}

void endutxent(void) {
  cursor = 0;
}

struct utmpx *getutxent(void) {
  init_entries();
  while (cursor < sizeof(entries) / sizeof(entries[0])) {
    struct utmpx *u = &entries[cursor++];
    if (u->ut_type != EMPTY)
      return u;
  }
  return NULL;
}

struct utmpx *getutxid(const struct utmpx *id) {
  size_t i;
  if (!id) {
    errno = EINVAL;
    return NULL;
  }
  init_entries();
  for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    if (entries[i].ut_type == id->ut_type && entries[i].ut_type != EMPTY)
      return &entries[i];
  }
  return NULL;
}

struct utmpx *getutxline(const struct utmpx *line) {
  size_t i;
  if (!line) {
    errno = EINVAL;
    return NULL;
  }
  init_entries();
  for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    if (entries[i].ut_type == USER_PROCESS || entries[i].ut_type == LOGIN_PROCESS) {
      if (strncmp(entries[i].ut_line, line->ut_line, sizeof(entries[i].ut_line)) == 0)
        return &entries[i];
    }
  }
  return NULL;
}

struct utmpx *pututxline(const struct utmpx *ut) {
  size_t i;
  if (!ut) {
    errno = EINVAL;
    return NULL;
  }
  init_entries();
  for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    if (entries[i].ut_type != EMPTY &&
        strncmp(entries[i].ut_line, ut->ut_line, sizeof(entries[i].ut_line)) == 0) {
      entries[i] = *ut;
      return &entries[i];
    }
  }
  for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    if (entries[i].ut_type == EMPTY) {
      entries[i] = *ut;
      return &entries[i];
    }
  }
  errno = ENOSPC;
  return NULL;
}
