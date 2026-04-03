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

#endif
