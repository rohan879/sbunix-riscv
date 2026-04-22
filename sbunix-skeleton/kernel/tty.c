#include <tty.h>
#include <uart.h>
#include <proc.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* TTY state                                                           */
/* ------------------------------------------------------------------ */

#define TTY_BUF_SIZE 256   /* cooked line buffer / ring buffer */

struct tty {
  struct termios termios;

  /* Cooked-mode line buffer (not a ring -- reset on line completion) */
  char   line_buf[TTY_BUF_SIZE];
  int    line_len;           /* chars in current line being edited */

  /* Completed-line ring buffer: lines are copied here when \n is received.
   * tty_read() consumes from this buffer. */
  char   cooked_buf[TTY_BUF_SIZE];
  int    cooked_r;           /* read position (ring) */
  int    cooked_w;           /* write position (ring) */
  int    cooked_eof;         /* ^D was pressed with empty line_buf */

  /* Raw-mode ring buffer */
  char   raw_buf[TTY_BUF_SIZE];
  int    raw_r;
  int    raw_w;

  /* Foreground process group — signals go here on ^C / ^Z */
  int    fg_pgid;

  /* Escape sequence state for cooked mode:
   *   0 = normal, 1 = saw ESC, 2 = saw ESC [ */
  int    esc_state;
};

static struct tty tty0;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Number of bytes available in a ring buffer */
static int ring_avail(int r, int w) {
  return (w - r + TTY_BUF_SIZE) % TTY_BUF_SIZE;
}

/* Free space in a ring buffer (reserve 1 slot to distinguish full/empty) */
static int ring_free(int r, int w) {
  return TTY_BUF_SIZE - 1 - ring_avail(r, w);
}

/* Enqueue one byte into a ring buffer.  Returns 0 on success, -1 if full. */
static int ring_put(char *buf, int *w, int r, char c) {
  if (ring_free(r, *w) == 0)
    return -1;
  buf[*w] = c;
  *w = (*w + 1) % TTY_BUF_SIZE;
  return 0;
}

/* Dequeue one byte from a ring buffer.  Returns 0 on success, -1 if empty. */
static int ring_get(char *buf, int *r, int w, char *out) {
  if (*r == w)
    return -1;
  *out = buf[*r];
  *r = (*r + 1) % TTY_BUF_SIZE;
  return 0;
}

/* Copy line_buf[0..line_len) into cooked_buf ring.
 * Drops characters if the ring is full. */
static void flush_line_to_cooked(void) {
  for (int i = 0; i < tty0.line_len; i++) {
    if (ring_put(tty0.cooked_buf, &tty0.cooked_w,
                 tty0.cooked_r, tty0.line_buf[i]) < 0) {
      /* Ring full -- ring bell and stop */
      uart_putc('\a');
      break;
    }
  }
  tty0.line_len = 0;
}

/* ------------------------------------------------------------------ */
/* tty_init                                                             */
/* ------------------------------------------------------------------ */

void tty_init(void) {
  memset(&tty0, 0, sizeof(tty0));

  tty0.termios.c_iflag = ICRNL;
  tty0.termios.c_oflag = OPOST | ONLCR;
  tty0.termios.c_cflag = 0;
  tty0.termios.c_lflag = ICANON | ECHO | ISIG;

  tty0.termios.c_cc[VINTR]  = 0x03;   /* ^C */
  tty0.termios.c_cc[VQUIT]  = 0x1C;   /* ^\ */
  tty0.termios.c_cc[VERASE] = 0x7F;   /* DEL (backspace) */
  tty0.termios.c_cc[VEOF]   = 0x04;   /* ^D */
  tty0.termios.c_cc[VSUSP]  = 0x1A;   /* ^Z */
  tty0.termios.c_cc[VMIN]   = 1;
  tty0.termios.c_cc[VTIME]  = 0;
}

/* Deliver a signal to the foreground process group.
 * If fg_pgid is 0 (not yet set), fall back to all processes pid > 1. */
static void tty_signal_fg(int sig) {
    int target_pgid = tty0.fg_pgid;
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED || p->state == ZOMBIE)
            continue;
        if (p->pid <= 1)
            continue;
        /* If a foreground group is set, only signal that group */
        if (target_pgid > 0 && p->pgid != target_pgid)
            continue;
        /* BF: Capture sender metadata for SA_SIGINFO delivery */
        sig_set_pending(p, sig, 0, 0, 0);
        if (p->state == SLEEPING)
            p->state = RUNNABLE;
    }
}

/* ------------------------------------------------------------------ */
/* tty_input -- called from UART interrupt handler (interrupt context)  */
/* ------------------------------------------------------------------ */

void tty_input(char c) {
  uint32_t lflag = tty0.termios.c_lflag;
  uint32_t iflag = tty0.termios.c_iflag;

  /* CR -> NL conversion */
  if ((iflag & ICRNL) && c == '\r')
    c = '\n';

  /* ---- Raw mode ---- */
  if (!(lflag & ICANON)) {
    /* ISIG handling in raw mode */
    if (lflag & ISIG) {
      if ((uint8_t)c == tty0.termios.c_cc[VINTR]) {
        tty_signal_fg(2); /* SIGINT */
        return;
      }
      if ((uint8_t)c == tty0.termios.c_cc[VQUIT]) {
        tty_signal_fg(3); /* SIGQUIT */
        return;
      }
      if ((uint8_t)c == tty0.termios.c_cc[VSUSP]) {
        tty_signal_fg(20); /* SIGTSTP */
        return;
      }
    }

    /* Enqueue into raw ring buffer */
    if (ring_put(tty0.raw_buf, &tty0.raw_w, tty0.raw_r, c) < 0)
      uart_putc('\a'); /* ring full */

    /* Echo if ECHO set */
    if (lflag & ECHO)
      uart_putc((int)(unsigned char)c);

    /* Wake sleeping reader */
    uart_wakeup();
    return;
  }

  /* ---- Cooked mode (ICANON set) ---- */

  /* Check for signal-generating characters */
  if (lflag & ISIG) {
    if ((uint8_t)c == tty0.termios.c_cc[VINTR]) {
      tty_signal_fg(2); /* SIGINT */
      if (lflag & ECHO) {
        uart_putc('^');
        uart_putc('C');
        uart_putc('\n');
      }
      tty0.line_len = 0;
      tty0.esc_state = 0;
      uart_wakeup();
      return;
    }

    if ((uint8_t)c == tty0.termios.c_cc[VQUIT]) {
      tty_signal_fg(3); /* SIGQUIT */
      if (lflag & ECHO) {
        uart_putc('^');
        uart_putc('\\');
        uart_putc('\n');
      }
      tty0.line_len = 0;
      tty0.esc_state = 0;
      uart_wakeup();
      return;
    }

    if ((uint8_t)c == tty0.termios.c_cc[VSUSP]) {
      tty_signal_fg(20); /* SIGTSTP */
      if (lflag & ECHO) {
        uart_putc('^');
        uart_putc('Z');
      }
      tty0.line_len = 0;
      tty0.esc_state = 0;
      uart_wakeup();
      return;
    }
  }

  /* ---- Escape sequence filtering (cooked mode) ----
   * Discard VT100 cursor/function key sequences (\033[...X) so that
   * pressing arrow keys doesn't insert garbage into the line buffer.
   * We do NOT do history recall — that would require storing history.
   * State machine:
   *   esc_state 0: normal
   *   esc_state 1: saw ESC (0x1B)
   *   esc_state 2: saw ESC [ — accumulating CSI sequence
   */
  if (c == 0x1B) {
    tty0.esc_state = 1;
    return; /* swallow ESC — don't add to line buffer */
  }
  if (tty0.esc_state == 1) {
    if (c == '[' || c == 'O') {
      /* CSI or SS3 — wait for the final byte */
      tty0.esc_state = 2;
    } else {
      /* Not a recognised sequence — discard ESC and this byte */
      tty0.esc_state = 0;
    }
    return;
  }
  if (tty0.esc_state == 2) {
    /* Final byte of a CSI/SS3 sequence is an ASCII letter (0x40–0x7E)
     * or a digit / semicolon for multi-parameter sequences.
     * We simply swallow everything until we see the final alpha byte. */
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
      tty0.esc_state = 0; /* sequence complete, still swallowed */
    }
    /* digits and ';' are intermediate — stay in state 2 */
    return;
  }

  /* Backspace / DEL */
  if ((uint8_t)c == tty0.termios.c_cc[VERASE] || c == 0x08) {
    if (tty0.line_len > 0) {
      tty0.line_len--;
      if (lflag & ECHO) {
        uart_putc('\b');
        uart_putc(' ');
        uart_putc('\b');
      }
    }
    return;
  }

  /* ^D (EOF) */
  if ((uint8_t)c == tty0.termios.c_cc[VEOF]) {
    if (tty0.line_len == 0) {
      /* Empty line: signal EOF */
      tty0.cooked_eof = 1;
      uart_wakeup();
    } else {
      /* Flush partial line without appending ^D or \n */
      flush_line_to_cooked();
      uart_wakeup();
    }
    return;
  }

  /* Newline: complete the line */
  if (c == '\n') {
    /* Append \n to line buffer */
    if (tty0.line_len < TTY_BUF_SIZE - 1)
      tty0.line_buf[tty0.line_len++] = '\n';
    /* Copy completed line into cooked ring */
    flush_line_to_cooked();
    if (lflag & ECHO)
      uart_putc('\n');
    uart_wakeup();
    return;
  }

  /* Regular character */
  if (tty0.line_len >= TTY_BUF_SIZE - 1) {
    /* Line buffer full */
    uart_putc('\a');
    return;
  }
  tty0.line_buf[tty0.line_len++] = c;
  if (lflag & ECHO)
    uart_putc((int)(unsigned char)c);
}

/* ------------------------------------------------------------------ */
/* tty_read -- called from process context (may sleep)                  */
/* ------------------------------------------------------------------ */

int tty_read(char *buf, int len) {
  if (len <= 0)
    return 0;

  uint32_t lflag = tty0.termios.c_lflag;

  if (lflag & ICANON) {
    /* ---- Cooked mode: wait for a complete line ---- */
    while (ring_avail(tty0.cooked_r, tty0.cooked_w) == 0 &&
           !tty0.cooked_eof) {
      /* Check for pending signals */
      if (current_proc->sigpending & ~current_proc->sigmask)
        return -4; /* -EINTR */
      current_proc->waiting_for_uart = 1;
      current_proc->state = SLEEPING;
      swtch(&current_proc->context, &scheduler_context);
      /* Re-check for signals after waking */
      if (current_proc->sigpending & ~current_proc->sigmask)
        return -4; /* -EINTR */
    }

    /* EOF? */
    if (tty0.cooked_eof && ring_avail(tty0.cooked_r, tty0.cooked_w) == 0) {
      tty0.cooked_eof = 0;
      return 0; /* EOF */
    }

    /* Copy up to len bytes, stopping at and including the first \n */
    int n = 0;
    while (n < len && ring_avail(tty0.cooked_r, tty0.cooked_w) > 0) {
      char ch = 0;
      ring_get(tty0.cooked_buf, &tty0.cooked_r, tty0.cooked_w, &ch);
      buf[n++] = ch;
      if (ch == '\n')
        break;
    }
    return n;

  } else {
    /* ---- Raw mode: return as soon as any data is available ---- */
    while (ring_avail(tty0.raw_r, tty0.raw_w) == 0) {
      /* Check for pending signals */
      if (current_proc->sigpending & ~current_proc->sigmask)
        return -4; /* -EINTR */
      current_proc->waiting_for_uart = 1;
      current_proc->state = SLEEPING;
      swtch(&current_proc->context, &scheduler_context);
      if (current_proc->sigpending & ~current_proc->sigmask)
        return -4; /* -EINTR */
    }

    int n = 0;
    while (n < len && ring_avail(tty0.raw_r, tty0.raw_w) > 0) {
      char ch = 0;
      ring_get(tty0.raw_buf, &tty0.raw_r, tty0.raw_w, &ch);
      buf[n++] = ch;
    }
    return n;
  }
}

/* ------------------------------------------------------------------ */
/* tty_ioctl                                                            */
/* ------------------------------------------------------------------ */

int tty_ioctl(unsigned long request, void *argp) {
  switch (request) {
  case TCGETS:
    memcpy(argp, &tty0.termios, sizeof(struct termios));
    return 0;

  case TCSETS:
  case TCSETSW:
    memcpy(&tty0.termios, argp, sizeof(struct termios));
    return 0;

  case TCSETSF:
    memcpy(&tty0.termios, argp, sizeof(struct termios));
    /* Flush input buffers */
    tty0.cooked_r = 0;
    tty0.cooked_w = 0;
    tty0.line_len = 0;
    tty0.raw_r = 0;
    tty0.raw_w = 0;
    tty0.cooked_eof = 0;
    return 0;

  case 0x540F: /* TIOCGPGRP */
    if (argp) {
      int *pgid_out = (int *)argp;
      *pgid_out = tty0.fg_pgid;
    }
    return tty0.fg_pgid;

  case 0x5410: /* TIOCSPGRP */
    if (argp) {
      int *pgid_in = (int *)argp;
      tty0.fg_pgid = *pgid_in;
    }
    return 0;

  default:
    return -25; /* -ENOTTY */
  }
}

int tty_fg_pgid(void) {
    return tty0.fg_pgid;
}
