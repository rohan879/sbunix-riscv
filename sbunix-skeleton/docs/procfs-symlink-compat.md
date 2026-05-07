# Procfs And Symlink Compatibility Contract

This document records the Linux-shaped procfs subset and POSIX Issue 8
symlink behavior implemented for SBUnix compatibility tests.

## Procfs

Existing procfs behavior is preserved for `/proc/self`, numeric PID
directories, `status`, `stat`, `cmdline`, `maps`, `fd`, `exe`, `cwd`,
`dmesg`, `mounts`, `cpuinfo`, `version`, `uptime`, `meminfo`, and
`loadavg`.

New process entries:

- `/proc/<pid>/stat` renders 52 Linux-shaped fields.
- `/proc/<pid>/statm` renders seven page-count fields followed by a newline.
- `/proc/<pid>/comm` renders the process basename truncated to 15 bytes plus
  a newline.
- `/proc/<pid>/environ` renders the exec-time environment as exact
  NUL-separated bytes with no appended NUL.
- `/proc/<pid>/root` is a readlink-compatible symlink to `/`.
- `/proc/<pid>/fdinfo/<n>` renders `pos`, octal `flags`, `mnt_id: 0`, and
  `ino` for open descriptors.

New global and diagnostic entries:

- `/proc/kmsg`
- `/proc/filesystems`
- `/proc/cmdline`
- `/proc/devices`
- `/proc/partitions`
- `/proc/consoles`
- `/proc/tty/drivers`
- `/proc/tty/ldiscs`
- `/proc/stat`
- `/proc/interrupts`
- `/proc/slabinfo`
- `/proc/buddyinfo`

The diagnostics prefer real counters where the kernel already tracks them.
Fields without backing accounting are zero-shaped placeholders, kept stable
for compatibility rather than pretending to be full Linux procfs.

All procfs regular reads support short reads, file offsets, EOF, and no
implicit NUL termination. Missing PIDs and missing descriptors return
`ENOENT`.

## Symlinks

Existing symlink resolution, `readlink`, `O_NOFOLLOW`,
`AT_SYMLINK_NOFOLLOW`, link/rename/unlink semantics, and the 40-hop limit are
preserved.

Compatibility policy:

- Symlink target text is opaque at creation time.
- Empty target text is allowed; `readlink()` returns `0`.
- Existing destination paths fail with `EEXIST` and are left untouched.
- Invalid `symlinkat()` directory file descriptors return `EBADF`.
- Non-directory `symlinkat()` directory file descriptors return `ENOTDIR`.
- New symlink targets are limited to `SYMLINK_MAX=255`; longer targets fail
  with `ENAMETOOLONG` instead of truncating.
- Newline characters in the destination basename are rejected with `EILSEQ`.
- New symlinks record uid/gid, initialize timestamps, and update parent
  directory timestamps after successful creation.
- `fchownat()` and `utimensat()` with `AT_SYMLINK_NOFOLLOW` update symlink
  metadata.
- `fchmodat()` with `AT_SYMLINK_NOFOLLOW` validates the symlink path and
  returns success without changing symlink permission bits.
