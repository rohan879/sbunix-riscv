# Termios / TTY Line Discipline Design

**Date**: 2026-03-08
**Branch**: `scheduler` (merge target: `main`)
**Status**: Design document -- no implementation code

## 1. Overview

The kernel currently passes raw UART bytes directly to userspace via a
128-byte ring buffer in `kernel/uart.c`.  All line editing (echo, backspace,
Ctrl-C) is performed by the shell in its `read_line()` function
(`bin/sbush/sbush.c:85-126`).  This design document describes a TTY line
discipline layer (`kernel/tty.c`) that interposes between the UART driver and
the `SYS_read` stdin path, providing cooked (canonical) and raw modes.

### Goals

1. Kernel handles echo, backspace, and signal-generating characters (^C, ^Z,
   ^D) in cooked mode -- the shell no longer needs to.
2. Raw mode passes bytes through with no buffering or editing, enabling
   programs like text editors to receive every keypress immediately.
3. A `struct termios` with `c_lflag` and ioctl(TCGETS/TCSETS) allow userspace
   to switch modes at runtime.

## 2. New Files

### kernel/tty.c

The sole new source file.  Contains the `struct tty`, line discipline logic,
and the public API.  Automatically compiled by the Makefile's
`$(wildcard kernel/*.c)` rule (Makefile line 17).

### kernel/include/tty.h

Header exposing the TTY API to `trap.c`, `devfs.c`, and `uart.c`.

## 3. Data Structures

### 3.1 struct termios (kernel-side, in tty.h)

```c
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
    uint8_t  c_cc[NCCS];       /* control characters */
};
```

### 3.2 struct tty (kernel-internal, in tty.c)

```c
#define TTY_BUF_SIZE 256   /* cooked line buffer */
#define TTY_RAW_SIZE 256   /* raw/pass-through ring buffer */

struct tty {
    struct termios termios;

    /* Cooked-mode line buffer (not a ring -- reset on line completion) */
    char   line_buf[TTY_BUF_SIZE];
    int    line_len;           /* chars in current line being edited */
    int    line_ready;         /* 1 when a complete line (or EOF) is available */

    /* Completed-line ring buffer: lines are copied here when \n is received.
     * tty_read() consumes from this buffer. */
    char   cooked_buf[TTY_BUF_SIZE];
    int    cooked_r;           /* read position (ring) */
    int    cooked_w;           /* write position (ring) */
    int    cooked_eof;         /* ^D was pressed with empty line_buf */

    /* Raw-mode ring buffer */
    char   raw_buf[TTY_RAW_SIZE];
    int    raw_r;
    int    raw_w;
};
```

Only a single global `struct tty tty0` is needed (one physical console).

### 3.3 Default termios values

```c
static struct termios default_termios = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = 0,
    .c_lflag = ICANON | ECHO | ISIG,
    .c_cc = {
        [VINTR]  = 0x03,   /* ^C */
        [VQUIT]  = 0x1C,   /* ^\ */
        [VERASE] = 0x7F,   /* DEL (backspace) */
        [VEOF]   = 0x04,   /* ^D */
        [VSUSP]  = 0x1A,   /* ^Z */
        [VMIN]   = 1,
        [VTIME]  = 0,
    },
};
```

## 4. Public API (tty.h)

```c
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
```

## 5. Detailed Behavior

### 5.1 tty_input(char c) -- interrupt context

This function replaces the current `uart_enqueue(c)` call in
`handle_external_interrupt()` (trap.c lines 219-220).

**Cooked mode (ICANON set):**

| Input | Action |
|-------|--------|
| Printable char | Append to `line_buf`. If ECHO: `uart_putc(c)`. |
| `\r` | Convert to `\n` (if ICRNL). Fall through to `\n` handling. |
| `\n` | Append `\n` to `line_buf`. Copy `line_buf[0..line_len]` into `cooked_buf` ring. Reset `line_len = 0`. Set `line_ready = 1`. If ECHO: `uart_putc('\n')`. Wake sleeping reader via `uart_wakeup()`. |
| `c_cc[VERASE]` (0x7F or 0x08) | If `line_len > 0`: decrement `line_len`. If ECHO: emit `"\b \b"` (backspace-space-backspace) via `uart_putc`. |
| `c_cc[VINTR]` (0x03) | If ISIG: deliver SIGINT to foreground process group (reuse existing ^C logic from trap.c lines 192-218). If ECHO: emit `"^C\n"`. Reset `line_buf` (discard partial input). Do NOT enqueue 0x03 into cooked_buf. |
| `c_cc[VSUSP]` (0x1A) | If ISIG: deliver SIGTSTP to foreground process group. If ECHO: emit `"^Z"`. Reset `line_buf`. |
| `c_cc[VEOF]` (0x04) | If `line_len == 0`: set `cooked_eof = 1`, wake reader (tty_read returns 0 = EOF). If `line_len > 0`: flush current `line_buf` as a completed line (without appending ^D itself), wake reader. Do NOT echo ^D. |
| Other control chars | Store in `line_buf` as-is. If ECHO, echo `^X` notation. |

**Raw mode (ICANON not set):**

| Input | Action |
|-------|--------|
| Any char | Enqueue into `raw_buf` ring. If ECHO: `uart_putc(c)`. If ISIG and char matches VINTR/VSUSP: deliver signal instead of enqueueing. Wake sleeping reader. |

### 5.2 tty_read(char *buf, int len) -- process context

**Cooked mode:**

1. While `cooked_r == cooked_w` and `cooked_eof == 0`:
   - Set `current_proc->waiting_for_uart = 1`
   - Set `current_proc->state = SLEEPING`
   - `swtch(&current_proc->context, &scheduler_context)`
   - On wake: check for pending signals (return -EINTR if any)
2. If `cooked_eof`: clear the flag, return 0 (EOF).
3. Copy up to `len` bytes from `cooked_buf[cooked_r..]` to `buf`, advancing
   `cooked_r`.  Stop at and include the first `\n`.  Return byte count.

**Raw mode:**

1. While `raw_r == raw_w`:
   - Sleep as above.
2. Copy up to `min(len, available)` bytes from `raw_buf` ring to `buf`.
   - If `VMIN > 0`: block until at least VMIN bytes are available (simplified:
     just return whatever is available, minimum 1 byte).
3. Return byte count.

### 5.3 tty_ioctl(request, argp)

| Request | Action |
|---------|--------|
| `TCGETS` (0x5401) | Copy `tty0.termios` to user address `argp`. |
| `TCSETS` (0x5402) | Copy `struct termios` from user address `argp` into `tty0.termios`. |
| `TCSETSW` (0x5403) | Same as TCSETS (no output queue to drain). |
| `TCSETSF` (0x5404) | Same as TCSETS, but also flush input buffers (`cooked_r = cooked_w = 0; line_len = 0; raw_r = raw_w = 0`). |
| Other | Return `-ENOTTY` (-25). |

User-to-kernel copy for argp reuses the existing `walk()` + PTE validation
pattern from the ioctl handler (trap.c lines 1539-1542).

## 6. File-by-File Changes

### 6.1 kernel/trap.c

**handle_external_interrupt() (lines 187-240):**

- Replace the `uart_enqueue(c)` call (line 220) and the entire ^C special-case
  block (lines 192-218) with a single call: `tty_input((char)c)`.
- Keep `last_key = (unsigned char)c` (line 222) for the status bar display.
- Keep `uart_wakeup()` call (line 224) -- or move it into `tty_input()` so
  the TTY layer controls when sleepers wake.
- Keep the waitpid-wakeup loop (lines 229-232) unchanged.

Before (lines 191-223):
```c
while ((c = uart_getc()) != -1) {
    if (c == 0x03) {
        /* ... 26 lines of SIGINT logic ... */
        if (!signalled_any)
            uart_enqueue(0x03);
    } else {
        uart_enqueue(c);
    }
    last_key = (unsigned char)c;
}
uart_wakeup();
```

After:
```c
while ((c = uart_getc()) != -1) {
    tty_input((char)c);
    last_key = (unsigned char)c;
}
/* tty_input() calls uart_wakeup() internally when data is ready */
```

**SYS_read stdin path (lines 803-836):**

- Replace the entire `if (fd == 0 && (!f || !f->is_pipe))` block (lines
  809-836) with a call to `tty_read()`:
  ```c
  if (fd == 0 && (!f || !f->is_pipe)) {
      int n = tty_read(buf, (int)len);
      tf->a0 = (n >= 0) ? (uint64_t)n : (uint64_t)-1;
  }
  ```
- This removes the uart_dequeue loop, the hardware polling fallback, and the
  `\r` to `\n` conversion (now handled by ICRNL in tty_input).

**SYS_ioctl handler (lines 1528-1555):**

- Add cases for TCGETS (0x5401), TCSETS (0x5402), TCSETSW (0x5403), TCSETSF
  (0x5404) before the catch-all:
  ```c
  } else if (request >= 0x5401 && request <= 0x5404) {
      /* termios get/set */
      if (fd >= 0 && fd <= 2) {
          pte_t *pte = walk(current_proc->pagetable, argp, 0);
          if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
              uint64_t pa = PTE2PA(*pte) + (argp & 0xFFF);
              tf->a0 = (uint64_t)tty_ioctl(request, (void *)pa);
          } else {
              tf->a0 = (uint64_t)-14; /* -EFAULT */
          }
      } else {
          tf->a0 = (uint64_t)-25; /* -ENOTTY */
      }
  }
  ```
- Add `#include <tty.h>` to the include block (after line 7).

### 6.2 kernel/devfs.c

**devconsole_read() (lines 39-43):**

- Currently returns 0 (stub).  Change to call `tty_read()`:
  ```c
  static int64_t devconsole_read(char *buf, uint64_t count) {
      return (int64_t)tty_read(buf, (int)count);
  }
  ```
- This makes `open("/dev/console")` + `read()` go through the line discipline,
  not just bare fd 0.
- Add `#include <tty.h>` to the include block (after line 4).

### 6.3 kernel/uart.c

- No changes to the UART driver itself.  The existing ring buffer
  (`ibuf[128]`) is kept but becomes vestigial once `tty_input()` replaces
  `uart_enqueue()` in the interrupt handler.  It can be removed in a follow-up
  cleanup commit, or kept as a fallback for direct hardware polling.
- `uart_putc()` remains the low-level character output function, called by
  `tty_input()` for echoing.

### 6.4 kernel/include/uart.h

- No changes needed.  `uart_putc()` is already declared.

### 6.5 bin/sbush/sbush.c

**read_line() (lines 85-126):**

Once cooked mode is active in the kernel, the shell's `read_line()` simplifies
to:
```c
static int read_line(char *buf, int max, const char *ps1) {
    write(STDOUT_FILENO, ps1, strlen(ps1));
    ssize_t n = read(STDIN_FILENO, buf, max - 1);
    if (n <= 0)
        return -1;  /* EOF or error */
    /* Strip trailing newline */
    if (n > 0 && buf[n - 1] == '\n')
        n--;
    buf[n] = '\0';
    return (int)n;
}
```

- The kernel handles echo, so no `write(STDOUT_FILENO, &c, 1)`.
- The kernel handles backspace editing, so no `"\x08 \x08"` logic.
- The kernel handles ^C by delivering SIGINT and discarding the line buffer.
  `read()` returns -1 with the signal pending; the shell's SIGINT handler runs.
  With `SA_RESTART`, the read auto-restarts, giving a clean prompt restart.
- The kernel handles ^D: `read()` returns 0 (EOF), and `read_line` returns -1.
- The `sigint_handler` (lines 23-29) remains useful for forwarding SIGINT to
  foreground children.

**Changes deferred**: The shell simplification is optional and can happen
in a separate commit after the kernel TTY layer is verified.  The existing
shell code is forward-compatible -- it will work (with double-echo) until
ECHO is tested and the shell code is trimmed.

### 6.6 kernel/kernel.c (boot function)

- Add a call to `tty_init()` during boot, after `uart_intr_init()` and before
  `userinit()`.

## 7. Integration Points Summary

```
UART HW ──► uart_getc()
               │
               ▼
        handle_external_interrupt()   [trap.c]
               │
               ▼
           tty_input(c)              [tty.c]  ◄── NEW entry point
           ┌───┴───────────┐
           │               │
      ICANON set      ICANON clear
           │               │
     ┌─────┴──────┐        │
     │  echo?     │        │
     │  signal?   │        │
     │  backspace?│        │
     │  newline?  │        │
     │  ^D?       │        │
     └─────┬──────┘        │
           │               │
           ▼               ▼
      cooked_buf       raw_buf
           │               │
           └───────┬───────┘
                   │
                   ▼
              tty_read()             [tty.c]  ◄── called from SYS_read
                   │
                   ▼
              userspace buf
```

## 8. Cooked Mode Line Editing Details

### Backspace behavior

When the user presses backspace (DEL 0x7F or BS 0x08):
1. If `line_len > 0`: decrement `line_len` (erasing the last character from
   the kernel's buffer).
2. If ECHO is set: emit the 3-byte sequence `\b \b` via `uart_putc` to
   visually erase the character on the terminal.
3. If `line_len == 0`: do nothing (cannot backspace past start of line).

### Ctrl-C (SIGINT) behavior

1. If ISIG is set in `c_lflag`:
   - Deliver SIGINT (signal 2) to all processes with pid > 1 that are not
     UNUSED or ZOMBIE (replicating the existing logic from trap.c lines
     200-209).
   - If ECHO: emit `"^C\n"` via `uart_putc`.
   - Discard current `line_buf` (`line_len = 0`).
   - Do NOT place 0x03 into `cooked_buf`.
2. If ISIG is not set: treat ^C as a regular character (store in buffer, echo
   if ECHO).

### Ctrl-Z (SIGTSTP) behavior

1. If ISIG is set:
   - Deliver SIGTSTP (signal 20) to the foreground process group.  In our
     simplified model, deliver to processes with pid > 1 (same as SIGINT
     delivery, but with signal 20 instead of 2).
   - If ECHO: emit `"^Z"` via `uart_putc`.
   - Discard current `line_buf`.
2. If ISIG is not set: treat ^Z as a regular character.

### Ctrl-D (EOF) behavior

1. If `line_len == 0` (empty line): signal EOF.  Set `cooked_eof = 1`.  Wake
   sleeping reader.  `tty_read()` returns 0.  Do not echo anything.
2. If `line_len > 0`: flush the partial line (copy `line_buf[0..line_len]`
   into `cooked_buf` without a trailing `\n`).  Wake sleeping reader.  The
   reader returns the partial line.  Do not echo ^D.

### Multi-line pasting

When text is pasted rapidly (multiple characters arrive in a single UART
interrupt burst), `tty_input()` is called for each character in sequence.
Lines are completed as `\n` characters arrive.  The `cooked_buf` ring can
hold multiple completed lines.  `tty_read()` returns one line at a time.

## 9. Edge Cases and Pitfalls

### 9.1 Interrupt context constraints

`tty_input()` runs in interrupt context (called from
`handle_external_interrupt`).  It must NOT:
- Sleep or call `swtch()`
- Allocate memory (no `kmalloc`)
- Acquire sleeping locks

It CAN:
- Write to `uart_putc()` for echo (busy-waits on UART TX ready, which is fast)
- Modify `tty0` fields (single-core, interrupts are disabled during handler)
- Set process state to RUNNABLE (wakeup)

### 9.2 Buffer overflow

- `line_buf` is 256 bytes.  If the user types 255 characters without pressing
  Enter, stop accepting characters and ring the terminal bell (`uart_putc('\a')`).
- `cooked_buf` ring is 256 bytes.  If it fills up (fast paste of multiple
  lines), drop characters and ring the bell.  This is a known limitation;
  a real OS would use a larger buffer or apply backpressure.

### 9.3 CR/NL conversion

The UART may deliver `\r` (0x0D) for the Enter key depending on the terminal
emulator.  When ICRNL is set in `c_iflag`, `tty_input()` converts `\r` to `\n`
before processing.  This replaces the ad-hoc `if (c == '\r') c = '\n'` on
trap.c line 831.

### 9.4 Race between tty_input() and tty_read()

`tty_input()` runs in interrupt context; `tty_read()` runs in process context.
On a single-core system (SBUnix is single-hart), the interrupt handler preempts
`tty_read()`.  This is safe because:
- `tty_input()` only advances `cooked_w` / `raw_w` (producer).
- `tty_read()` only advances `cooked_r` / `raw_r` (consumer).
- Single-producer, single-consumer ring buffers are safe without locks on
  single-core when the producer and consumer use separate indices.

If multi-hart support is added later, a spinlock around the TTY struct will be
needed.

### 9.5 Signal delivery and read() restart

When ISIG is active and ^C is pressed:
- `tty_input()` delivers SIGINT and discards the line buffer.
- The sleeping reader is woken.
- `tty_read()` should check `current_proc->sigpending` after waking and
  return -EINTR (value: -4) if a signal is pending.
- The shell's `sigaction` for SIGINT uses `SA_RESTART`, so the libc `read()`
  wrapper should automatically retry.  If libc does not yet implement
  SA_RESTART retry logic, the shell's existing `read_line` loop will handle it.

### 9.6 Double echo during transition

While the kernel has ECHO enabled and the shell still has its own echo logic
in `read_line()`, characters will appear twice.  The shell simplification
(section 6.5) should be deployed alongside or immediately after the kernel
TTY layer.  Alternatively, the shell can call `ioctl(TCGETS)` to check whether
ECHO is set and skip its own echo if so -- but it is simpler to just remove
the shell echo code.

### 9.7 ^D with pending partial line across page boundary

When `tty_read()` copies bytes to the user buffer, it must validate the user
pointer via `walk()` + PTE checks (as done elsewhere in trap.c).  If the
user buffer spans a page boundary, each page must be validated.  For
simplicity, the initial implementation can limit copies to within a single
page (4096 bytes) since line buffers are at most 256 bytes.

### 9.8 Foreground process group

The current ^C logic (trap.c lines 200-209) broadcasts SIGINT to all
processes with pid > 1.  A proper TTY layer should only signal the foreground
process group.  For now, retain the broadcast approach.  A future enhancement
can add a `fg_pgrp` field to `struct tty` and use TIOCSPGRP/TIOCGPGRP
(already stubbed at trap.c lines 1549-1552) to manage it.

### 9.9 Cooked buffer not a simple ring for line semantics

The cooked_buf ring must track line boundaries so `tty_read()` returns at most
one complete line per call (POSIX canonical read semantics).  Two approaches:
- **Simple**: copy entire lines including `\n` into the ring; `tty_read()`
  scans for `\n` in the ring and returns up to that point.
- **Indexed**: maintain a separate count of complete lines.  Decrement when
  a line is fully consumed.

The simple approach (scan for `\n`) is recommended for this teaching OS.

## 10. Libc Support

### 10.1 New header: libc/include/termios.h

```c
#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <sys/types.h>

/* Flag and struct definitions (mirror kernel tty.h values) */
#define ICANON  0x0002
#define ECHO    0x0008
#define ISIG    0x0001

#define TCGETS  0x5401
#define TCSETS  0x5402

#define NCCS    20

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[NCCS];
};

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int optional_actions, const struct termios *t);

#endif
```

### 10.2 New source: libc/termios.c

Thin wrappers:
```c
int tcgetattr(int fd, struct termios *t) {
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int optional_actions, const struct termios *t) {
    unsigned long req = TCSETS;
    if (optional_actions == 1) req = TCSETSW;  /* TCSADRAIN */
    if (optional_actions == 2) req = TCSETSF;  /* TCSAFLUSH */
    return ioctl(fd, req, (void *)t);
}
```

## 11. Testing Approach

### 11.1 Basic cooked mode (no code changes to shell)

1. Boot kernel with TTY layer.  Shell's `read_line()` still has its own echo.
2. **Expect**: double echo (both kernel and shell echo each character).
3. This confirms `tty_input()` echo and `tty_read()` line delivery work.

### 11.2 Shell simplification

1. Simplify shell's `read_line()` as described in section 6.5.
2. Type characters -- should appear once (kernel echo only).
3. Press backspace -- character should be visually erased.
4. Press Enter -- command executes.
5. Press ^C -- `"^C\n"` appears, fresh prompt shown.
6. Press ^D on empty line -- shell exits (EOF).
7. Type partial input, press ^D -- partial input is delivered as a short read.

### 11.3 Raw mode test program (bin/rawtest/rawtest.c)

A small test program that:
1. Calls `tcgetattr(0, &old)` to save current settings.
2. Clears ICANON and ECHO in `c_lflag`.
3. Calls `tcsetattr(0, 0, &new)` to enter raw mode.
4. Loops: `read(0, &c, 1)` and `printf("got: 0x%02x '%c'\n", c, c)`.
5. On 'q': restores old termios and exits.

Expected behavior: every keypress immediately produces output, no echo, no
line buffering, no special character processing (except ISIG if left on).

### 11.4 Signal tests

1. In cooked mode, run a long-running program (e.g., `spin`).
2. Press ^C -- program should receive SIGINT and terminate.
3. Press ^Z -- program should receive SIGTSTP and stop (if job control
   is implemented) or terminate with default SIGTSTP action.

### 11.5 Regression tests

1. Pipes: `echo hello | cat` -- should still work (pipe reads bypass TTY).
2. Script mode: `sbush script.sh` -- reads from file, not TTY.  Verify
   that `fd 0` backed by a pipe/file does not go through `tty_read()`.
3. Background jobs and signal delivery still function correctly.

### 11.6 Edge case tests

1. Type 255 characters without Enter -- 256th should ring bell, not overflow.
2. Rapid paste of multi-line input -- all lines should be delivered correctly.
3. ^D after partial input -- verify short read contains the typed characters.
4. Backspace at start of line -- nothing happens, no corruption.

## 12. Implementation Order

1. **Create `kernel/include/tty.h`** with struct definitions and API.
2. **Create `kernel/tty.c`** with `tty_init()`, `tty_input()`, `tty_read()`,
   `tty_ioctl()`.
3. **Modify `kernel/trap.c`**: wire interrupt handler to `tty_input()`,
   replace SYS_read stdin path with `tty_read()`, add TCGETS/TCSETS ioctl
   cases.
4. **Modify `kernel/devfs.c`**: wire `devconsole_read()` to `tty_read()`.
5. **Modify `kernel/kernel.c`**: add `tty_init()` call in boot sequence.
6. **Test cooked mode** with existing shell (expect double echo).
7. **Add libc/include/termios.h and libc/termios.c**.
8. **Simplify `bin/sbush/sbush.c` read_line()** to remove echo/editing.
9. **Test clean cooked mode** (single echo from kernel).
10. **Write `bin/rawtest/rawtest.c`** and test raw mode.
11. **Cleanup**: optionally remove the vestigial `ibuf` ring buffer from
    `kernel/uart.c` if no longer needed.
