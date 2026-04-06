#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

typedef long           ssize_t;
typedef unsigned long  size_t;
typedef int            pid_t;
typedef long           off_t;
typedef long           time_t;
typedef long           clock_t;
typedef unsigned int   mode_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
typedef unsigned int   id_t;
typedef unsigned long  dev_t;
typedef unsigned long  ino_t;
typedef long           ino64_t;
typedef unsigned int   nlink_t;
typedef int            blksize_t;
typedef long           blkcnt_t;

#ifndef __suseconds_t_defined
typedef long suseconds_t;
#define __suseconds_t_defined
#endif

#ifndef __clockid_t_defined
typedef int clockid_t;
#define __clockid_t_defined
#endif

#ifndef __timer_t_defined
typedef void *timer_t;
#define __timer_t_defined
#endif

#ifndef __fsblkcnt_t_defined
typedef unsigned long fsblkcnt_t;
#define __fsblkcnt_t_defined
#endif

#ifndef __fsfilcnt_t_defined
typedef unsigned long fsfilcnt_t;
#define __fsfilcnt_t_defined
#endif

#ifndef __key_t_defined
typedef int key_t;
#define __key_t_defined
#endif

#ifndef __reclen_t_defined
typedef unsigned short reclen_t;
#define __reclen_t_defined
#endif

/* ENH: POSIX requires pthread types to be available from <sys/types.h>.
 * These are minimal type definitions for a single-threaded OS that allow
 * test binaries and POSIX-conformant code to compile. */
#ifndef __pthread_t_defined
typedef unsigned long pthread_t;
#define __pthread_t_defined
#endif

#ifndef __pthread_attr_t_defined
typedef struct { int __detachstate; size_t __stacksize; } pthread_attr_t;
#define __pthread_attr_t_defined
#endif

#ifndef __pthread_mutex_t_defined
typedef struct { int __lock; } pthread_mutex_t;
#define __pthread_mutex_t_defined
#endif

#ifndef __pthread_mutexattr_t_defined
typedef struct { int __type; } pthread_mutexattr_t;
#define __pthread_mutexattr_t_defined
#endif

#ifndef __pthread_cond_t_defined
typedef struct { int __dummy; } pthread_cond_t;
#define __pthread_cond_t_defined
#endif

#ifndef __pthread_condattr_t_defined
typedef struct { int __clock; } pthread_condattr_t;
#define __pthread_condattr_t_defined
#endif

#ifndef __pthread_rwlock_t_defined
typedef struct { int __lock; } pthread_rwlock_t;
#define __pthread_rwlock_t_defined
#endif

#ifndef __pthread_rwlockattr_t_defined
typedef struct { int __pshared; } pthread_rwlockattr_t;
#define __pthread_rwlockattr_t_defined
#endif

#ifndef __pthread_key_t_defined
typedef unsigned int pthread_key_t;
#define __pthread_key_t_defined
#endif

#ifndef __pthread_once_t_defined
typedef int pthread_once_t;
#define __pthread_once_t_defined
#endif

#ifndef __pthread_barrier_t_defined
typedef struct { int __count; } pthread_barrier_t;
#define __pthread_barrier_t_defined
#endif

#ifndef __pthread_barrierattr_t_defined
typedef struct { int __pshared; } pthread_barrierattr_t;
#define __pthread_barrierattr_t_defined
#endif

#ifndef __pthread_spinlock_t_defined
typedef volatile int pthread_spinlock_t;
#define __pthread_spinlock_t_defined
#endif

#endif
