/*
 * termios.c — terminal I/O control for SBUnix libc.
 *
 * tcgetattr/tcsetattr wrap ioctl TCGETS/TCSETS which the kernel
 * handles in tty_ioctl().
 */

#include <termios.h>
#include <sys/ioctl.h>

/* TCGETS = 0x5401, TCSETS = 0x5402, TCSETSW = 0x5403, TCSETSF = 0x5404 */
#define TCGETS  0x5401
#define TCSETS  0x5402

int tcgetattr(int fd, struct termios *termios_p) {
    return ioctl(fd, TCGETS, termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    unsigned long req = (unsigned long)(TCSETS + optional_actions);
    return ioctl(fd, req, (void *)termios_p);
}

void cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(const struct termios *termios_p) {
    (void)termios_p;
    return B115200;
}

speed_t cfgetospeed(const struct termios *termios_p) {
    (void)termios_p;
    return B115200;
}

int cfsetispeed(struct termios *termios_p, speed_t speed) {
    (void)termios_p; (void)speed;
    return 0;
}

int cfsetospeed(struct termios *termios_p, speed_t speed) {
    (void)termios_p; (void)speed;
    return 0;
}
