#ifndef _SYS_IPC_H
#define _SYS_IPC_H

#include <sys/types.h>

#define IPC_PRIVATE ((key_t)0)

#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000

#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2

struct ipc_perm {
  key_t __key;
  uid_t uid;
  gid_t gid;
  uid_t cuid;
  gid_t cgid;
  mode_t mode;
  unsigned short __seq;
  unsigned long __unused1;
  unsigned long __unused2;
};

#endif /* _SYS_IPC_H */
