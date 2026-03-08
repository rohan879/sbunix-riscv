#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <stdint.h>

/* Type aliases */
typedef unsigned int tcflag_t;
typedef unsigned int speed_t;
typedef unsigned char cc_t;

/* Special control character indices — must match kernel tty.h */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSUSP    10
#define NCCS     20

/* c_iflag bits */
#define IGNBRK   0x0001
#define BRKINT   0x0002
#define IGNPAR   0x0004
#define PARMRK   0x0008
#define INPCK    0x0010
#define ISTRIP   0x0020
#define INLCR    0x0040
#define ICRNL    0x0100
#define IUCLC    0x0200
#define IXON     0x0400
#define IXANY    0x0800
#define IGNCR    0x0080
#define IXOFF    0x1000

/* c_oflag bits */
#define OPOST    0x0001
#define ONLCR    0x0004

/* c_cflag bits */
#define CSIZE    0x0030
#define CS5      0x0000
#define CS6      0x0010
#define CS7      0x0020
#define CS8      0x0030
#define CSTOPB   0x0040
#define CREAD    0x0080
#define PARENB   0x0100
#define PARODD   0x0200
#define HUPCL    0x0400
#define CLOCAL   0x0800

/* c_lflag bits */
#define ISIG     0x0001
#define ICANON   0x0002
#define ECHO     0x0008
#define ECHOE    0x0010
#define ECHOK    0x0020
#define ECHONL   0x0040
#define NOFLSH   0x0080
#define TOSTOP   0x0100
#define IEXTEN   0x8000

/* tcsetattr actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* Baud rates (placeholders) */
#define B0       0
#define B9600    9600
#define B19200   19200
#define B38400   38400
#define B115200  115200

/* struct termios — must match kernel tty.h exactly */
struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[NCCS];
};

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
void cfmakeraw(struct termios *termios_p);
speed_t cfgetispeed(const struct termios *termios_p);
speed_t cfgetospeed(const struct termios *termios_p);
int cfsetispeed(struct termios *termios_p, speed_t speed);
int cfsetospeed(struct termios *termios_p, speed_t speed);

#endif /* _TERMIOS_H */
