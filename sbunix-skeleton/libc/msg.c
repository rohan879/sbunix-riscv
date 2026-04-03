#include <sys/msg.h>
#include <syscall.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MSGQ_MAX 16
#define MSGQ_BUF 4096

struct msgq_local {
  int used;
  key_t key;
  struct msqid_ds ds;
  unsigned long head;
  unsigned long tail;
  unsigned long bytes;
  unsigned char data[MSGQ_BUF];
};

static struct msgq_local queues[MSGQ_MAX];

static int alloc_queue(key_t key, int msgflg) {
  int i;
  for (i = 0; i < MSGQ_MAX; i++) {
    if (!queues[i].used) {
      memset(&queues[i], 0, sizeof(queues[i]));
      queues[i].used = 1;
      queues[i].key = key;
      queues[i].ds.msg_perm.__key = key;
      queues[i].ds.msg_perm.uid = getuid();
      queues[i].ds.msg_perm.gid = getgid();
      queues[i].ds.msg_perm.cuid = getuid();
      queues[i].ds.msg_perm.cgid = getgid();
      queues[i].ds.msg_perm.mode = (mode_t)(msgflg & 0777);
      queues[i].ds.msg_qbytes = MSGQ_BUF;
      return i + 1;
    }
  }
  errno = ENOSPC;
  return -1;
}

static struct msgq_local *lookup_queue(int msqid) {
  if (msqid <= 0 || msqid > MSGQ_MAX || !queues[msqid - 1].used) {
    errno = EINVAL;
    return NULL;
  }
  return &queues[msqid - 1];
}

int msgget(key_t key, int msgflg) {
  int i;
  if (key != IPC_PRIVATE) {
    for (i = 0; i < MSGQ_MAX; i++) {
      if (queues[i].used && queues[i].key == key) {
        if ((msgflg & IPC_EXCL) && (msgflg & IPC_CREAT)) {
          errno = EEXIST;
          return -1;
        }
        return i + 1;
      }
    }
    if (!(msgflg & IPC_CREAT)) {
      errno = ENOENT;
      return -1;
    }
  }
  return alloc_queue(key, msgflg);
}

int msgctl(int msqid, int cmd, struct msqid_ds *buf) {
  struct msgq_local *q = lookup_queue(msqid);
  if (!q)
    return -1;
  switch (cmd) {
    case IPC_STAT:
      if (!buf) {
        errno = EFAULT;
        return -1;
      }
      *buf = q->ds;
      return 0;
    case IPC_RMID:
      memset(q, 0, sizeof(*q));
      return 0;
    case IPC_SET:
      if (!buf) {
        errno = EFAULT;
        return -1;
      }
      q->ds.msg_perm.uid = buf->msg_perm.uid;
      q->ds.msg_perm.gid = buf->msg_perm.gid;
      q->ds.msg_perm.mode = buf->msg_perm.mode;
      q->ds.msg_qbytes = buf->msg_qbytes;
      return 0;
    default:
      errno = EINVAL;
      return -1;
  }
}

int msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg) {
  struct msgq_local *q = lookup_queue(msqid);
  const unsigned char *src = (const unsigned char *)msgp;
  size_t total = sizeof(long) + sizeof(size_t) + msgsz;
  unsigned char hdr[sizeof(long) + sizeof(size_t)];
  size_t i;
  long type;
  size_t payload = msgsz;
  (void)msgflg;
  if (!q)
    return -1;
  if (!msgp) {
    errno = EFAULT;
    return -1;
  }
  memcpy(&type, msgp, sizeof(long));
  if (type <= 0) {
    errno = EINVAL;
    return -1;
  }
  if (total > MSGQ_BUF - q->bytes) {
    errno = EAGAIN;
    return -1;
  }
  memcpy(hdr, &type, sizeof(long));
  memcpy(hdr + sizeof(long), &payload, sizeof(size_t));
  for (i = 0; i < sizeof(hdr); i++) {
    q->data[q->tail] = hdr[i];
    q->tail = (q->tail + 1) % MSGQ_BUF;
  }
  for (i = 0; i < msgsz; i++) {
    q->data[q->tail] = src[sizeof(long) + i];
    q->tail = (q->tail + 1) % MSGQ_BUF;
  }
  q->bytes += total;
  q->ds.__msg_cbytes = q->bytes;
  q->ds.msg_qnum++;
  q->ds.msg_lspid = getpid();
  q->ds.msg_stime = time(NULL);
  return 0;
}

ssize_t msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg) {
  struct msgq_local *q = lookup_queue(msqid);
  unsigned char frame[sizeof(long) + sizeof(size_t)];
  unsigned char new_data[MSGQ_BUF];
  unsigned long pos = q ? q->head : 0;
  unsigned long remaining = q ? q->bytes : 0;
  unsigned long out_tail = 0;
  int found = 0;
  size_t recv_len = 0;
  size_t i;
  if (!q)
    return -1;
  if (!msgp) {
    errno = EFAULT;
    return -1;
  }
  while (remaining >= sizeof(long) + sizeof(size_t)) {
    long type;
    size_t payload;
    for (i = 0; i < sizeof(long); i++)
      frame[i] = q->data[(pos + i) % MSGQ_BUF];
    memcpy(&type, frame, sizeof(long));
    for (i = 0; i < sizeof(size_t); i++)
      frame[sizeof(long) + i] = q->data[(pos + sizeof(long) + i) % MSGQ_BUF];
    memcpy(&payload, frame + sizeof(long), sizeof(size_t));
    if (remaining < sizeof(long) + sizeof(size_t) + payload)
      break;
    if (!found && (msgtyp == 0 || msgtyp == type)) {
      if (payload > msgsz) {
        if (!(msgflg & MSG_NOERROR)) {
          errno = E2BIG;
          return -1;
        }
        recv_len = msgsz;
      } else {
        recv_len = payload;
      }
      memcpy(msgp, &type, sizeof(long));
      for (i = 0; i < recv_len; i++)
        ((unsigned char *)msgp)[sizeof(long) + i] =
          q->data[(pos + sizeof(long) + sizeof(size_t) + i) % MSGQ_BUF];
      found = 1;
    } else {
      for (i = 0; i < sizeof(long) + sizeof(size_t) + payload; i++) {
        new_data[out_tail++] = q->data[(pos + i) % MSGQ_BUF];
      }
    }
    pos = (pos + sizeof(long) + sizeof(size_t) + payload) % MSGQ_BUF;
    remaining -= sizeof(long) + sizeof(size_t) + payload;
  }
  if (!found) {
    errno = ENOMSG;
    return -1;
  }
  memset(q->data, 0, sizeof(q->data));
  memcpy(q->data, new_data, out_tail);
  q->head = 0;
  q->tail = out_tail % MSGQ_BUF;
  q->bytes = out_tail;
  q->ds.__msg_cbytes = q->bytes;
  if (q->ds.msg_qnum > 0)
    q->ds.msg_qnum--;
  q->ds.msg_lrpid = getpid();
  q->ds.msg_rtime = time(NULL);
  return (ssize_t)recv_len;
}
