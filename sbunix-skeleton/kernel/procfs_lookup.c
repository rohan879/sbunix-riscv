#include <procfs_internal.h>
#include <string.h>
#include <errno.h>

/* RF: Tiny lookup helpers live outside procfs.c so new procfs renderers can
 * grow without making the VFS dispatch file harder to audit. */
int procfs_global_type_from_name(const char *name, enum procfs_type *type) {
  if (strcmp(name, "kmsg") == 0) { *type = PROCFS_KMSG; return 0; }
  if (strcmp(name, "filesystems") == 0) { *type = PROCFS_FILESYSTEMS; return 0; }
  if (strcmp(name, "cmdline") == 0) { *type = PROCFS_CMDLINE_GLOBAL; return 0; }
  if (strcmp(name, "devices") == 0) { *type = PROCFS_DEVICES; return 0; }
  if (strcmp(name, "partitions") == 0) { *type = PROCFS_PARTITIONS; return 0; }
  if (strcmp(name, "consoles") == 0) { *type = PROCFS_CONSOLES; return 0; }
  if (strcmp(name, "tty/drivers") == 0) { *type = PROCFS_TTY_DRIVERS; return 0; }
  if (strcmp(name, "tty/ldiscs") == 0) { *type = PROCFS_TTY_LDISCS; return 0; }
  if (strcmp(name, "stat") == 0) { *type = PROCFS_STAT_GLOBAL; return 0; }
  if (strcmp(name, "interrupts") == 0) { *type = PROCFS_INTERRUPTS; return 0; }
  if (strcmp(name, "slabinfo") == 0) { *type = PROCFS_SLABINFO; return 0; }
  if (strcmp(name, "buddyinfo") == 0) { *type = PROCFS_BUDDYINFO; return 0; }
  return -ENOENT;
}

uint64_t procfs_global_ino(enum procfs_type type) {
  return 8000 + (uint64_t)type;
}
