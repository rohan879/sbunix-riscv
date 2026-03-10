# TTY/Kernel Correctness Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix two kernel bugs — the TTY doesn't track its foreground process group, and Ctrl-C/Ctrl-Z broadcast signals to all processes instead of just the foreground group.

**Architecture:** Add a `fg_pgid` field to the `struct tty` in the kernel. Wire TIOCSPGRP to store it, TIOCGPGRP to return it, and replace the four signal-broadcast loops in `tty_input()` with a helper that only targets processes whose `pgid` matches `fg_pgid`.

**Tech Stack:** C (kernel-space, freestanding, riscv64-unknown-elf-gcc)

---

## Task 1: Add `fg_pgid` field to the TTY struct and wire TIOCSPGRP/TIOCGPGRP

**Files:**
- Modify: `kernel/tty.c:12-30` (struct tty — add field)
- Modify: `kernel/tty.c:84-99` (tty_init — initialize field)
- Modify: `kernel/tty.c:317-342` (tty_ioctl — add cases)
- Modify: `kernel/include/tty.h:43-57` (declare new ioctl constants and expose getter)
- Modify: `kernel/trap.c:1706-1709` (remove old TIOCGPGRP/TIOCSPGRP stubs)

### Step 1: Add `fg_pgid` to `struct tty`

In `kernel/tty.c`, add a field to the struct at line 30 (before the closing brace):

```c
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
};
```

### Step 2: Initialize `fg_pgid` in `tty_init()`

In `kernel/tty.c` function `tty_init()`, the `memset(&tty0, 0, sizeof(tty0))` at line 85 already zeroes it. No extra code needed — `fg_pgid == 0` means "no foreground group set yet". Signals fall back to broadcasting to all if `fg_pgid == 0`.

### Step 3: Add TIOCSPGRP / TIOCGPGRP to `tty_ioctl()`

In `kernel/tty.c`, add two new cases to `tty_ioctl()` (inside the switch, before the `default:` at line 339):

```c
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
```

### Step 4: Add ioctl constants and getter to `kernel/include/tty.h`

Add after the existing TCSETSF define (line 22):

```c
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
```

Add a getter function declaration after `tty_ioctl` (line 56):

```c
/* Return the foreground process group ID stored in the TTY */
int tty_fg_pgid(void);
```

Then implement it in `kernel/tty.c` (after `tty_ioctl`):

```c
int tty_fg_pgid(void) {
    return tty0.fg_pgid;
}
```

### Step 5: Remove old TIOCGPGRP/TIOCSPGRP stubs from `kernel/trap.c`

In `kernel/trap.c` at lines 1706-1709, replace the two stubs:

```c
    } else if (request == 0x540F) { /* TIOCGPGRP */
        return (long)current_proc->pid;
    } else if (request == 0x5410) { /* TIOCSPGRP */
        return (long)0;
```

With code that routes through the TTY's ioctl handler, passing the userspace pointer:

```c
    } else if (request == 0x540F || request == 0x5410) {
        /* TIOCGPGRP / TIOCSPGRP — delegate to tty_ioctl */
        return (long)tty_ioctl(request, (void *)arg);
```

Here `arg` is the third argument to the ioctl syscall (the pointer to an `int` in userspace). Check how the ioctl handler extracts `arg` — it should be `tf->a2` (the third syscall argument). Verify that the existing code already extracts it (look for the variable name used around line 1685).

### Step 6: Build and verify it compiles

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build with zero errors/warnings.

### Step 7: Commit

```
BF: Fixed TIOCSPGRP to store foreground pgid in tty struct
```

---

## Task 2: Scope signal delivery to the foreground process group

**Files:**
- Modify: `kernel/tty.c:105-246` (tty_input — replace four broadcast loops)

### Step 1: Add a `tty_signal_fg()` helper function

Add this helper in `kernel/tty.c`, above `tty_input()` (before line 105):

```c
/* Deliver a signal to the foreground process group.
 * If fg_pgid is 0 (not yet set), fall back to all processes pid > 1. */
static void tty_signal_fg(int sig) {
    int target_pgid = tty0.fg_pgid;
    for (struct proc *p = proc; p < &proc[64]; p++) {
        if (p->state == UNUSED || p->state == ZOMBIE)
            continue;
        if (p->pid <= 1)
            continue;
        /* If a foreground group is set, only signal that group */
        if (target_pgid > 0 && p->pgid != target_pgid)
            continue;
        p->sigpending |= (1UL << sig);
        if (p->state == SLEEPING)
            p->state = RUNNABLE;
    }
}
```

### Step 2: Replace the four broadcast loops in `tty_input()` with calls to `tty_signal_fg()`

**Raw-mode SIGINT** (lines 117-126): Replace:
```c
      if ((uint8_t)c == tty0.termios.c_cc[VINTR]) {
        /* Deliver SIGINT to all processes pid > 1 */
        for (struct proc *p = proc; p < &proc[64]; p++) {
          if (p->state != UNUSED && p->state != ZOMBIE && p->pid > 1) {
            p->sigpending |= (1UL << 2); /* SIGINT */
            if (p->state == SLEEPING)
              p->state = RUNNABLE;
          }
        }
        return;
      }
```
With:
```c
      if ((uint8_t)c == tty0.termios.c_cc[VINTR]) {
        tty_signal_fg(2); /* SIGINT */
        return;
      }
```

**Raw-mode SIGTSTP** (lines 128-138): Replace the loop with:
```c
      if ((uint8_t)c == tty0.termios.c_cc[VSUSP]) {
        tty_signal_fg(20); /* SIGTSTP */
        return;
      }
```

**Cooked-mode SIGINT** (lines 158-175): Replace the loop with:
```c
    if ((uint8_t)c == tty0.termios.c_cc[VINTR]) {
      tty_signal_fg(2); /* SIGINT */
      if (lflag & ECHO) {
        uart_putc('^');
        uart_putc('C');
        uart_putc('\n');
      }
      tty0.line_len = 0;
      uart_wakeup();
      return;
    }
```

**Cooked-mode SIGTSTP** (lines 178-194): Replace the loop with:
```c
    if ((uint8_t)c == tty0.termios.c_cc[VSUSP]) {
      tty_signal_fg(20); /* SIGTSTP */
      if (lflag & ECHO) {
        uart_putc('^');
        uart_putc('Z');
      }
      tty0.line_len = 0;
      uart_wakeup();
      return;
    }
```

### Step 3: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 4: Test in QEMU

Run: `make qemu`

Test session in the shell:
```
sbush$ sleep 999 &
[1] <pid>
sbush$ jobs
[1] Running    sleep 999
sbush$ ^C
sbush$ jobs
[1] Running    sleep 999
sbush$ fg 1
sleep 999
^C
sbush$ jobs
sbush$
```

**What to verify:**
1. `sleep 999 &` starts a background job
2. Pressing Ctrl-C at the prompt does NOT kill the background sleep
3. `jobs` after Ctrl-C still shows the sleep as Running
4. `fg 1` brings the sleep to the foreground
5. Ctrl-C while sleep is in foreground kills it
6. `jobs` now shows nothing

**Fallback behavior:** If `fg_pgid` is 0 (e.g. at the prompt with no foreground pipeline), the signal falls back to broadcasting to all processes pid > 1. This matches the old behavior and is correct because the shell itself ignores SIGINT (it has a handler that forwards to `fg_pid`).

### Step 5: Commit

```
BF: Scoped Ctrl-C and Ctrl-Z signals to foreground process group
```
