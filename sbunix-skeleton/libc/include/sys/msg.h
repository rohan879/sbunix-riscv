#ifndef _SYS_MSG_H
#define _SYS_MSG_H

#include <sys/types.h>
#include <sys/ipc.h>

typedef unsigned long msgqnum_t;
typedef unsigned long msglen_t;

struct msqid_ds {
  struct ipc_perm msg_perm;
  time_t msg_stime;
  time_t msg_rtime;
  time_t msg_ctime;
  unsigned long __msg_cbytes;
  msgqnum_t msg_qnum;
  msglen_t msg_qbytes;
  pid_t msg_lspid;
  pid_t msg_lrpid;
  unsigned long __msg_pad1;
  unsigned long __msg_pad2;
};

#define MSG_NOERROR 010000

int msgget(key_t key, int msgflg);
int msgctl(int msqid, int cmd, struct msqid_ds *buf);
int msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg);
ssize_t msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg);

#endif /* _SYS_MSG_H */
