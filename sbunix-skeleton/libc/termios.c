/*
 * termios.c — terminal I/O control for SBUnix libc.
 *
 * tcgetattr/tcsetattr wrap ioctl TCGETS/TCSETS which the kernel
 * handles in tty_ioctl().
 */

#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>

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

int tcflush(int fd, int queue_selector) {
    return ioctl(fd, 0x540B /* TCFLSH */, queue_selector);
}

int tcdrain(int fd) {
    return ioctl(fd, 0x5409 /* TCSBRK */, 1);
}

int tcflow(int fd, int action) {
    return ioctl(fd, 0x540A /* TCXONC */, action);
}

int tcsendbreak(int fd, int duration) {
    return ioctl(fd, 0x5409 /* TCSBRK */, duration);
}

pid_t tcgetsid(int fd) {
    pid_t sid = 0;
    if (ioctl(fd, TIOCGSID, &sid) < 0)
        return -1;
    return sid;
}

int tcgetwinsize(int fd, struct winsize *ws) {
    if (!ws) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, TIOCGWINSZ, ws);
}

int tcsetwinsize(int fd, const struct winsize *ws) {
    if (!ws) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, TIOCSWINSZ, (void *)ws);
}
