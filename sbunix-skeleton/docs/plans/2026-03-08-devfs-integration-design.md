# Design: Route fd 0/1/2 Through /dev/console VFS Ops

**Date:** 2026-03-08
**Branch:** scheduler
**Status:** Design (not yet implemented)

## 1. Problem Statement

Currently, file descriptors 0, 1, and 2 (stdin, stdout, stderr) are special-cased
throughout `trap.c`. They are set up in `init_starter()` (proc.c, lines 53-66) with
`f->ops = NULL` -- explicitly flagged as "UART console, not VFS-backed." The syscall
handlers in `trap.c` then check for these magic fd numbers and route reads/writes
directly to UART hardware, bypassing the VFS entirely.

This creates several problems:
- The SYS_read handler (trap.c:809) checks `fd == 0 && (!f || !f->is_pipe)` and
  inlines the entire UART dequeue/sleep loop.
- The SYS_write handler (trap.c:763) checks `(fd == 1 || fd == 2) && (!f || !f->is_pipe)`
  and inlines a printk-per-byte output loop.
- SYS_fstat (trap.c:1313) checks `fd <= 2 && (!f || !f->ops)` to synthesize a
  character-device stat result.
- SYS_ioctl / TIOCGWINSZ (trap.c:1534) checks `fd >= 0 && fd <= 2` to decide
  "is a terminal."
- These checks are fragile: they break if a shell redirects fd 0/1/2 via dup2 to a
  pipe or file, because the checks test the fd *number* rather than the file *type*.

## 2. Goal

Replace all fd-number-based UART special-casing with proper VFS dispatch through
`/dev/console`. After this change:
- `init_starter()` opens `/dev/console` three times via VFS to populate fds 0, 1, 2.
- `devconsole_read()` contains the UART input logic (dequeue, sleep, wakeup).
- `devconsole_write()` calls `uart_write()` for each byte.
- SYS_read and SYS_write fall through to the existing VFS dispatch path for all fds
  (no fd-number checks remain).
- fork() inherits these fds automatically (refcount bumped via existing `copy_ofile`).
- dup/dup2 works naturally (shared `open_file` with `ops == &devfs_ops`).

## 3. Files to Change

### 3.1 kernel/devfs.c (lines 38-48)

**What:** Rewrite `devconsole_read()` and `devconsole_write()`.

**devconsole_read() (currently lines 39-43):**
The current implementation is a stub that returns 0. It must be replaced with the
UART input logic currently inlined in `trap.c` SYS_read (lines 809-836).

New behavior:
```
devconsole_read(char *buf, uint64_t count):
  i = 0
  while i < count:
    while uart_dequeue(&c) != 0:
      hw_c = uart_getc()           // poll hardware directly
      if hw_c != -1:
        uart_enqueue((char)hw_c)
        continue
      // sleep until UART interrupt
      current_proc->waiting_for_uart = 1
      current_proc->state = SLEEPING
      swtch(&current_proc->context, &scheduler_context)
    if c == '\r': c = '\n'
    buf[i++] = c
    break                          // return after one character
  return i
```

This requires `devconsole_read()` to include headers for `current_proc`, `swtch`,
`scheduler_context`, `uart_getc`, `uart_enqueue`, and `uart_dequeue`. Since devfs.c
already includes `<uart.h>`, the UART functions are available. The process/scheduler
symbols need new includes (see section 4).

**devconsole_write() (currently lines 44-48):**
Currently calls `printk("%c", buf[i])` per byte. This works but is indirect. Replace
with direct `uart_write()` calls for efficiency and to remove the dependency on
printk's format parsing:

```
devconsole_write(const char *buf, uint64_t count):
  for i = 0 .. count-1:
    uart_write(buf[i])
  return count
```

`uart_write()` is already declared in `<uart.h>` which devfs.c already includes.

### 3.2 kernel/devfs.c -- new ioctl-like support (optional, for fstat/isatty)

Add a boolean flag or device-type field to `struct device`:

```c
struct device {
  const char *name;
  int64_t (*read_fn)(char *buf, uint64_t count);
  int64_t (*write_fn)(const char *buf, uint64_t count);
  int is_tty;    /* NEW: 1 if this device is a terminal */
};
```

Set `is_tty = 1` for the "console" entry, 0 for "null" and "zero". This lets
SYS_ioctl and SYS_fstat query the open_file's device type instead of checking fd
numbers.

### 3.3 kernel/devfs.c -- expose a helper or extend vfs_ops

**Option A (minimal):** Store the `struct device *` pointer in `open_file->private`
(which it already is, via `devfs_private`). The ioctl handler in trap.c can check
whether `f->ops == &devfs_ops` and then cast `f->private` to `struct devfs_private *`
to inspect `priv->dev->is_tty`.

**Option B (cleaner):** Add an `ioctl` function pointer to `struct vfs_ops`:

```c
int (*ioctl)(void *private, unsigned long request, uint64_t arg);
```

Then implement `devfs_ioctl()` that handles TIOCGWINSZ for console devices. This is
cleaner long-term but a larger change. Recommend Option A for now, Option B as
follow-up.

### 3.4 kernel/proc.c -- init_starter() (lines 44-70)

**What:** Replace the manual `vfs_alloc_file()` loop (lines 53-66) with three
VFS open calls.

Current code (lines 53-66):
```c
for (int i = 0; i < 3; i++) {
    struct open_file *f = vfs_alloc_file();
    if (f) {
        f->ops = NULL; /* UART console, not VFS-backed */
        ...
    }
    current_proc->ofile[i] = f;
}
```

Replace with a helper that performs the equivalent of `open("/dev/console", O_RDWR)`.
Because `init_starter()` runs in kernel context (before the first sret to user mode),
we cannot issue an ecall. Instead, call the VFS layer directly:

```c
static int kern_open_console(void) {
    const char *relpath;
    struct vfs_ops *ops = vfs_lookup_mount("/dev/console", &relpath);
    if (!ops || !ops->open) return -1;
    void *priv = NULL;
    int ret = ops->open(relpath, 2 /* O_RDWR */, &priv);
    if (ret < 0) return ret;
    struct open_file *f = vfs_alloc_file();
    if (!f) { ops->close(priv); return -12; }
    f->ops = ops;
    f->private = priv;
    f->offset = 0;
    f->flags = 2; /* O_RDWR */
    f->is_pipe = 0;
    return <fd>;
}
```

This function should be placed in proc.c (or a shared vfs helper). For the three
opens, either:
- Call it three times (each gets its own `devfs_private` allocation and `open_file`
  with independent refcount = 1), OR
- Call it once, then dup twice (sharing one `open_file` with refcount = 3).

**Recommendation:** Three separate opens. This matches POSIX semantics where
stdin/stdout/stderr are independently closeable. With separate `open_file` entries,
closing fd 0 does not affect fd 1 or fd 2.

The loop in `init_starter()` becomes:
```c
for (int i = 0; i < 3; i++) {
    int fd = kern_open_console();
    /* fd will be i because ofile[0..2] are still NULL at this point */
    if (fd < 0)
        printk("init_starter: failed to open /dev/console for fd %d\n", i);
}
```

**New includes needed in proc.c:** None beyond what is already included (`<vfs.h>`
is already there).

### 3.5 kernel/trap.c -- SYS_write (lines 757-801)

**What:** Remove the `fd == 1 || fd == 2` special case.

Current dispatch order (lines 763-800):
1. `(fd == 1 || fd == 2) && (!f || !f->is_pipe)` -> printk per byte (UART)
2. `f && f->is_pipe && f->pipe_write` -> pipe write
3. `f && f->ops && f->ops->write` -> VFS write
4. else -> return -1

New dispatch order:
1. `f && f->is_pipe && f->pipe_write` -> pipe write
2. `f && f->ops && f->ops->write` -> VFS write (this now handles console too)
3. else -> return -1

Delete lines 763-768 entirely. Reorder so pipe check comes first, then VFS.

### 3.6 kernel/trap.c -- SYS_read (lines 803-866)

**What:** Remove the `fd == 0` special case.

Current dispatch order (lines 809-865):
1. `fd == 0 && (!f || !f->is_pipe)` -> inline UART dequeue loop
2. `f && f->is_pipe && f->pipe_read` -> pipe read
3. `f && f->ops && f->ops->read` -> VFS read
4. else -> return 0

New dispatch order:
1. `f && f->is_pipe && f->pipe_read` -> pipe read
2. `f && f->ops && f->ops->read` -> VFS read (this now handles console too)
3. else -> return 0

Delete lines 809-836 entirely. The UART dequeue/sleep logic moves to
`devconsole_read()` in devfs.c (section 3.1).

### 3.7 kernel/trap.c -- SYS_fstat (lines 1300-1330)

**What:** Remove the `fd <= 2 && (!f || !f->ops)` special case at line 1313.

After this change, fds 0/1/2 will have `f->ops == &devfs_ops`, so they will fall
through to the generic VFS fstat path (lines 1319-1329). However, that path currently
returns `S_IFREG | 0644` for all VFS files. Console fds should return `S_IFCHR`.

**Fix:** Add an `fstat` function pointer to `struct vfs_ops`, or use a simpler
approach: check `f->ops == &devfs_ops` and call `devfs_stat` on the device. But we
don't have the path in the fstat context.

**Better approach:** Store the stat mode in `struct device` or in `devfs_private`:

```c
struct devfs_private {
  struct device *dev;
  uint32_t st_mode;   /* cached from devfs_stat */
};
```

Set `st_mode` during `devfs_open()`. Then in SYS_fstat, the VFS fstat path can
check if `f->ops` has a per-fd stat capability. Alternatively, add an `fstat`
op to `vfs_ops`:

```c
int (*fstat)(void *private, uint8_t *st);
```

Implement `devfs_fstat()` that fills in `S_IFCHR | 0666` for device files.
This is the cleanest path.

Delete lines 1313-1318 from trap.c. Add the fstat dispatch:
```c
} else if (f->ops && f->ops->fstat) {
    int ret = f->ops->fstat(f->private, st);
    tf->a0 = (uint64_t)(int64_t)ret;
} else {
    /* generic regular file fallback */
    ...
}
```

### 3.8 kernel/trap.c -- SYS_ioctl / TIOCGWINSZ (lines 1528-1555)

**What:** Replace the `fd >= 0 && fd <= 2` check (line 1534) with a device-type
check.

Using Option A from section 3.3:
```c
if (request == 0x5413) { /* TIOCGWINSZ */
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? current_proc->ofile[fd] : NULL;
    int is_tty = 0;
    if (f && f->ops == &devfs_ops && f->private) {
        struct devfs_private *priv = (struct devfs_private *)f->private;
        if (priv->dev && priv->dev->is_tty)
            is_tty = 1;
    }
    if (is_tty) {
        /* write winsize struct ... */
        tf->a0 = 0;
    } else {
        tf->a0 = (uint64_t)-25; /* -ENOTTY */
    }
}
```

This requires making `struct devfs_private` visible to trap.c. Either:
- Move the struct definition to a header (e.g., `devfs.h`), or
- Provide a `devfs_is_tty(void *private)` helper function declared in a header.

**Recommendation:** Add a small header `kernel/include/devfs.h` or expose a single
function `int devfs_is_console(void *private)` in `vfs.h` / declared extern in
trap.c. The helper approach avoids leaking devfs internals.

### 3.9 kernel/include/vfs.h (lines 10-19)

**What:** Optionally extend `struct vfs_ops` with new function pointers.

Add (if pursuing the clean approach):
```c
int (*fstat)(void *private, uint8_t *st);  /* per-fd stat */
int (*ioctl)(void *private, unsigned long request, uint64_t arg);  /* optional */
```

These are optional fields (NULL means unsupported). Existing filesystems (tarfs) set
them to NULL and continue working.

## 4. New Structs, Functions, and Headers

### New functions

| Function | File | Purpose |
|---|---|---|
| `kern_open_console()` | proc.c | Open /dev/console via VFS, return fd |
| `devfs_is_tty(void *private)` | devfs.c | Return 1 if the open device is a terminal |
| `devfs_fstat(void *private, uint8_t *st)` | devfs.c | Fill stat buf for an open device fd |

### Modified functions

| Function | File | Change |
|---|---|---|
| `devconsole_read()` | devfs.c | Stub -> full UART dequeue/sleep logic |
| `devconsole_write()` | devfs.c | printk loop -> uart_write loop |
| `init_starter()` | proc.c | Manual ofile setup -> VFS open calls |
| trap_handler SYS_read | trap.c | Remove fd==0 special case |
| trap_handler SYS_write | trap.c | Remove fd==1/2 special case |
| trap_handler SYS_fstat | trap.c | Remove fd<=2 special case |
| trap_handler SYS_ioctl | trap.c | Replace fd<=2 check with device-type query |

### New includes needed

- **devfs.c** needs: `<proc.h>` (for `current_proc`, `swtch`, `scheduler_context`,
  `struct proc`). It already has `<uart.h>`.
- **trap.c** needs: access to `devfs_is_tty()` -- either via a new `<devfs.h>` or
  by declaring it extern.

### Struct modifications

| Struct | Field | Purpose |
|---|---|---|
| `struct device` (devfs.c) | `int is_tty` | Mark console as a terminal device |
| `struct vfs_ops` (vfs.h) | `int (*fstat)(...)` | Per-fd stat for device files |

## 5. Integration Points with Existing Code

### 5.1 fork / copy_ofile (trap.c, lines 69-85)

No changes needed. `copy_ofile()` already increments `f->refcount` for every
non-NULL `ofile[i]`. Since console fds will now have valid `ops` and `private`
pointers, they will be properly shared across fork. The `devfs_private` allocation
is per-open (not per-fd), so the shared `open_file` correctly points to one
`devfs_private`.

### 5.2 close_ofile (trap.c, lines 88-124)

No changes needed. When refcount drops to zero, `close_ofile` already calls
`f->ops->close(f->private)`, which will invoke `devfs_close()` to free the
`devfs_private` struct. Currently this path is skipped for console fds because
`f->ops == NULL`. After the change, it will work correctly.

### 5.3 exec (trap.c SYS_exec path)

Exec does not close fds in this OS (matching Unix semantics where fds survive exec
unless close-on-exec is set). No changes needed.

### 5.4 exit / signal default handler (trap.c lines 1387-1397, lines 557-562)

Both paths iterate `ofile[0..NOFILE]` and call `close_ofile()`. No changes needed --
the VFS close path already handles ops-backed files.

### 5.5 dup3 (trap.c lines 928-953)

Already works generically: bumps refcount on the shared `open_file`. No fd-number
checks. No changes needed.

### 5.6 alloc_fd (trap.c lines 61-67)

Returns the lowest free slot. After `init_starter()` opens fds 0-2 via VFS, slots
0-2 are occupied. Subsequent `alloc_fd()` calls return 3+. No changes needed.

### 5.7 devfs mount (vfs.c lines 104-105)

`/dev` is already mounted as devfs. `vfs_lookup_mount("/dev/console", &relpath)` will
match the `/dev` mount and set `relpath = "console"`. This is already working for
userspace `open("/dev/console")` calls. No changes needed.

### 5.8 printk (kernel/printk.c)

`printk()` calls `uart_write()` directly. It is NOT routed through the VFS console
and should remain independent -- it is a kernel debug facility that must work even
before VFS is initialized. No changes needed.

## 6. Edge Cases and Pitfalls

### 6.1 Sleep/wakeup in devconsole_read()

The UART dequeue loop calls `swtch()` to yield the CPU when no input is available.
This requires access to `current_proc`, `scheduler_context`, and the `swtch()`
function. These are process-layer symbols. Including them in devfs.c creates a
dependency from the "device layer" to the "process layer."

**Mitigation:** This is acceptable in a teaching OS. The alternative (a callback
or wait-queue abstraction) is overengineering for this context. Document the
dependency clearly in a comment.

### 6.2 Interrupt context safety

`devconsole_read()` is called from the syscall handler (trap context). It calls
`swtch()` to sleep, which is only safe in process context (not interrupt context).
This is the same pattern as the current inline code, so no new risk is introduced.

### 6.3 Console close and reopen

If a process closes fd 0 and reopens it (e.g., redirecting stdin from a file, then
restoring it), the `close_ofile()` path will call `devfs_close()`, freeing the
`devfs_private`. A subsequent `open("/dev/console")` creates a new `devfs_private`.
This is correct behavior.

### 6.4 Multiple readers on /dev/console

If multiple processes have /dev/console open for reading (e.g., after fork), both
will compete for UART input via `uart_dequeue()`. The ring buffer is a shared
resource with no per-reader tracking, so characters go to whichever process dequeues
first. This matches current behavior and is acceptable (in practice, only one shell
reads stdin at a time).

### 6.5 init_starter ordering: exec before fd setup

Currently, `init_starter()` calls `exec()` first (line 45), then sets up fds
(lines 56-66). This order matters because `exec()` calls `allocproc()` internally
or sets up the address space, and the fds must be set on `current_proc` after it is
fully configured.

**Pitfall:** If we change fd setup to use VFS open (which calls `devfs_open` ->
`kmalloc`), we must ensure the kernel heap is initialized. Since `boot()` calls
`kinit()` and `vfs_init()` before `userinit()`, and `init_starter()` runs even
later (when the scheduler first picks the init process), the heap is ready. No issue.

**Pitfall:** The VFS open call in `init_starter()` runs with `current_proc` already
pointing to the init process. `alloc_fd()` needs `current_proc` to be set. Since
`init_starter()` is called as the init process's entry point (via `context.ra`),
`current_proc` is valid. Confirmed safe.

### 6.6 devconsole_write using uart_write vs printk

The current `devconsole_write()` uses `printk("%c", buf[i])`, which is wasteful
(format string parsing per character). Switching to `uart_write(buf[i])` is a
direct improvement. However, note that `uart_write()` waits for the transmit
holding register to be empty (busy-waits on UART_LSR). For large writes, this is
blocking. The current code has the same behavior via printk, so no regression.

### 6.7 Pipe redirection still works

The pipe check in SYS_read/SYS_write uses `f->is_pipe` (not fd numbers). After a
shell does `dup2(pipe_write_end, 1)`, fd 1's `open_file` has `is_pipe = 1` and
`ops = NULL`. The pipe path fires. The VFS path (which would call
`devconsole_write`) is never reached. This is correct.

After the change, the dispatch order in SYS_write becomes:
1. pipe check (`f->is_pipe`) -> pipe write
2. VFS check (`f->ops && f->ops->write`) -> includes console
3. else -> error

This correctly handles: console fds (path 2), pipe-redirected fds (path 1), and
invalid fds (path 3).

### 6.8 devfs_private visibility for ioctl

The `struct devfs_private` is currently file-static in devfs.c. The ioctl handler
in trap.c needs to determine if an fd refers to a terminal device. Rather than
exposing the struct, provide a helper:

```c
/* In devfs.c, declared in a header */
int devfs_is_tty(void *private) {
    struct devfs_private *priv = (struct devfs_private *)private;
    return (priv && priv->dev && priv->dev->is_tty) ? 1 : 0;
}
```

trap.c calls: `if (f->ops == &devfs_ops && devfs_is_tty(f->private))`.

### 6.9 O_RDONLY / O_WRONLY semantics

Currently, all three fds are opened with the same flags. Strictly, fd 0 should be
O_RDONLY and fds 1-2 should be O_WRONLY. However, the devfs implementation ignores
flags (the `flags` parameter to `devfs_open` is unused). For a teaching OS, opening
all three as O_RDWR is fine and matches Linux behavior for `/dev/console`.

## 7. Testing Approach

### 7.1 Basic smoke test

Boot the kernel and verify the shell prompt appears. Type commands and confirm
they echo correctly and produce output. This validates:
- `devconsole_write()` works (shell prompt appears)
- `devconsole_read()` works (keystrokes are delivered)
- fd inheritance across fork+exec works (init -> sbush)

### 7.2 Pipe redirection test

Run a command with pipe redirection (e.g., `echo hello | cat`). This validates:
- dup2 on fd 1 still works (stdout redirected to pipe)
- dup2 on fd 0 still works (stdin redirected from pipe)
- Pipe fds take priority over console in dispatch

### 7.3 File redirection test (if shell supports it)

Run `cat < /etc/some_file`. Validates that fd 0 can be replaced with a VFS file
and the VFS read path handles it.

### 7.4 fstat / isatty test

Write a small test program:
```c
#include <unistd.h>
#include <sys/stat.h>

int main() {
    struct stat st;
    fstat(0, &st);
    /* Check S_ISCHR(st.st_mode) is true */
    fstat(1, &st);
    /* Check S_ISCHR(st.st_mode) is true */
    return 0;
}
```

### 7.5 Close and reopen test

```c
close(0);
int fd = open("/dev/console", O_RDONLY);
/* fd should be 0 */
/* read from fd should work */
```

### 7.6 Fork inheritance test

```c
pid_t pid = fork();
if (pid == 0) {
    write(1, "child\n", 6);  /* should go to console */
    _exit(0);
}
write(1, "parent\n", 7);     /* should also go to console */
```

### 7.7 Regression: existing programs

Run all existing test binaries (`spin`, `alarmtest`, `segvtest`) and the shell
(`sbush`). Ensure no regressions in output or input handling.

## 8. Implementation Order

1. **devfs.c**: Add `is_tty` to `struct device`, implement real `devconsole_read()`
   and `devconsole_write()`, add `devfs_is_tty()` helper and `devfs_fstat()`.
2. **vfs.h**: Add `fstat` function pointer to `struct vfs_ops`. Add `devfs_is_tty()`
   declaration.
3. **proc.c**: Rewrite `init_starter()` fd setup to open `/dev/console` via VFS.
4. **trap.c SYS_write**: Remove fd==1/2 special case (delete lines 763-768).
5. **trap.c SYS_read**: Remove fd==0 special case (delete lines 809-836).
6. **trap.c SYS_fstat**: Remove fd<=2 special case (delete lines 1313-1318),
   add fstat ops dispatch.
7. **trap.c SYS_ioctl**: Replace fd<=2 check with `devfs_is_tty()` call.
8. **Test**: Boot, verify shell works, run pipe test, run existing test binaries.

## 9. Lines of Code Estimate

| File | Lines removed | Lines added | Net |
|---|---|---|---|
| devfs.c | ~8 (stubs) | ~45 (read logic, write, is_tty, fstat) | +37 |
| vfs.h | 0 | ~4 (fstat op, is_tty decl) | +4 |
| proc.c | ~14 (manual fd setup) | ~25 (kern_open_console + loop) | +11 |
| trap.c | ~35 (special cases) | ~8 (fstat/ioctl dispatch) | -27 |
| **Total** | ~57 | ~82 | **+25** |

Net result: roughly 25 new lines, cleaner architecture, and elimination of all
fd-number-based UART special-casing.
