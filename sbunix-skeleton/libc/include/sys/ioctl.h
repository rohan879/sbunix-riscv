#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <sys/types.h>

/* Terminal window size */
struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

/* ioctl request codes */
#define TIOCGWINSZ  0x5413  /* get window size */
#define TIOCSWINSZ  0x5414  /* set window size */
#define TIOCGPGRP   0x540F  /* get foreground process group */
#define TIOCSPGRP   0x5410  /* set foreground process group */
#define TIOCGSID    0x5429  /* get controlling session id */
#define FIONREAD    0x541B  /* bytes available to read */

int ioctl(int fd, unsigned long request, ...);

#endif /* _SYS_IOCTL_H */
