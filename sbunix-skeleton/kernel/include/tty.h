#ifndef _TTY_H
#define _TTY_H

#include <riscv.h>

/* Flags for c_lflag */
#define ICANON  0x0002   /* canonical (cooked) mode */
#define ECHO    0x0008   /* echo input characters */
#define ISIG    0x0001   /* enable signal-generating characters */

/* Flags for c_iflag */
#define ICRNL   0x0100   /* map CR to NL on input */

/* Flags for c_oflag */
#define OPOST   0x0001   /* enable output processing */
#define ONLCR   0x0004   /* map NL to CR-NL on output */

/* ioctl request codes (Linux values) */
#define TCGETS  0x5401
#define TCSETS  0x5402
#define TCSETSW 0x5403   /* drain output, then set */
#define TCSETSF 0x5404   /* drain output, flush input, then set */

/* Special control character indices */
#define VINTR    0   /* ^C  -> SIGINT  */
#define VQUIT    1   /* ^\  -> SIGQUIT */
#define VERASE   2   /* ^?  -> backspace */
#define VEOF     4   /* ^D  -> EOF */
#define VSUSP    10  /* ^Z  -> SIGTSTP */
#define VMIN     6   /* raw mode: min chars before read returns */
#define VTIME    5   /* raw mode: timeout in deciseconds */
#define NCCS     20

struct termios {
  uint32_t c_iflag;           /* input mode flags */
  uint32_t c_oflag;           /* output mode flags */
  uint32_t c_cflag;           /* control mode flags (unused for now) */
  uint32_t c_lflag;           /* local mode flags */
  uint8_t  c_cc[NCCS];        /* control characters */
};

/* Initialization -- called from boot() */
void tty_init(void);

/* Called from UART interrupt handler; feeds one character into the
 * line discipline.  Runs in interrupt context, must not sleep. */
void tty_input(char c);

/* Called from SYS_read when fd is stdin/console.  May sleep (puts
 * current_proc to SLEEPING and calls swtch).  Returns number of
 * bytes copied into buf, or 0 for EOF. */
int tty_read(char *buf, int len);

/* ioctl dispatch -- called from SYS_ioctl handler.
 * Returns 0 on success, negative errno on failure. */
int tty_ioctl(unsigned long request, void *argp);

#endif
