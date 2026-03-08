# Kernel Syscall Stubs Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add kernel-side handlers for syscalls that libc already wraps but the kernel ignores (printing "Unknown syscall"). This unblocks libc functions like `isatty()`, `sigaction()`, `gettimeofday()`, `access()`, and fixes `execve` envp passthrough.

**Architecture:** All changes in `kernel/trap.c` syscall dispatch (if-else chain at lines 514-1475) and `kernel/include/proc.h` for per-process signal state. Kernel-only changes.

**Tech Stack:** C (freestanding kernel), `kernel/include/` headers, RISC-V Sv39 page tables.

**Key reference:** The syscall dispatch is a linear if-else chain in `kernel/trap.c`. The catch-all "Unknown syscall" is at lines ~1473-1474. New handlers go before that catch-all. Each handler reads args from `tf->a0`-`tf->a5` (trapframe registers) and writes the return value to `tf->a0`.

---

## Task 1: Add `SYS_ioctl` handler (enables `isatty`)

**Files:**
- Modify: `kernel/trap.c` (add handler before "Unknown syscall" catch-all)
- Modify: `kernel/include/syscall.h` (verify `SYS_ioctl 29` exists)

`isatty()` works by calling `ioctl(fd, TIOCGWINSZ, &ws)`. If the fd is a terminal, return 0 (success). If not, return -ENOTTY.

**Step 1: Add the handler**

In `kernel/trap.c`, before the "Unknown syscall" block, add:

```c
else if (num == 29) { /* SYS_ioctl */
    int fd = (int)tf->a0;
    unsigned long request = tf->a1;
    uint64_t argp = tf->a2;

    if (request == 0x5413) { /* TIOCGWINSZ */
        if (fd >= 0 && fd <= 2) {
            /* stdin/stdout/stderr are the UART terminal */
            if (argp) {
                /* Write a default 80x24 winsize struct to user memory */
                struct { uint16_t row, col, xpix, ypix; } ws = {24, 80, 0, 0};
                /* Copy to user space */
                pte_t *pte = walk(current_proc->pagetable, argp, 0);
                if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                    uint64_t pa = PTE2PA(*pte) + (argp & 0xFFF);
                    memcpy((void *)pa, &ws, sizeof(ws));
                }
            }
            tf->a0 = 0; /* success — it IS a terminal */
        } else {
            tf->a0 = (uint64_t)-25; /* -ENOTTY */
        }
    } else if (request == 0x540F) { /* TIOCGPGRP */
        tf->a0 = (uint64_t)current_proc->pid;
    } else if (request == 0x5410) { /* TIOCSPGRP */
        tf->a0 = 0;
    } else {
        tf->a0 = (uint64_t)-25; /* -ENOTTY for unknown ioctls */
    }
}
```

**Step 2: Build and verify**

Run: `cd sbunix-skeleton && source tools/sourceme.sh && make clean && make`
Expected: Compiles with no errors.

**Step 3: Test in QEMU**

Run: `make qemu`
Verify: Shell boots. No "Unknown syscall 29" messages in output. `isatty()` should now return 1 for fd 0/1/2.

**Step 4: Commit**

```bash
git add kernel/trap.c
git commit -m "ENH: Added SYS_ioctl handler for isatty and TIOCGWINSZ"
```

---

## Task 2: Add `SYS_rt_sigaction` handler

**Files:**
- Modify: `kernel/include/proc.h` (add sigaction table to struct proc)
- Modify: `kernel/proc.c` (zero sigaction table in allocproc)
- Modify: `kernel/trap.c` (add handler)

This stores user-registered signal handlers per process. Actual signal *delivery* is in Workstream 4 — this task just stores/retrieves the sigaction structs so the syscall doesn't fail.

**Step 1: Add signal state to `struct proc`**

In `kernel/include/proc.h`, add before the closing `};` of `struct proc`:

```c
  /* Signal handling */
  struct kern_sigaction {
      uint64_t handler;   /* sa_handler function pointer */
      uint64_t mask;      /* sa_mask */
      int      flags;     /* sa_flags */
      uint64_t restorer;  /* sa_restorer */
  } sigactions[32];       /* one per signal (indices 0-31) */
  uint64_t sigmask;       /* blocked signal mask */
  uint64_t sigpending;    /* pending signal bitmask */
```

**Step 2: Zero signal state in `allocproc()`**

In `kernel/proc.c`, in the `allocproc()` function, after existing initialization, add:

```c
  memset(p->sigactions, 0, sizeof(p->sigactions));
  p->sigmask = 0;
  p->sigpending = 0;
```

**Step 3: Add SYS_rt_sigaction handler in `kernel/trap.c`**

Before the "Unknown syscall" catch-all:

```c
else if (num == 134) { /* SYS_rt_sigaction */
    int signum = (int)tf->a0;
    uint64_t act_uaddr = tf->a1;    /* const struct sigaction *act */
    uint64_t oldact_uaddr = tf->a2; /* struct sigaction *oldact */
    /* a3 = sigsetsize, ignored */

    if (signum < 1 || signum >= 32 || signum == 9 || signum == 19) {
        tf->a0 = (uint64_t)-22; /* -EINVAL */
    } else {
        /* Return old action if requested */
        if (oldact_uaddr) {
            struct { uint64_t handler; uint64_t mask; int flags; int pad; uint64_t restorer; } old;
            old.handler = current_proc->sigactions[signum].handler;
            old.mask = current_proc->sigactions[signum].mask;
            old.flags = current_proc->sigactions[signum].flags;
            old.pad = 0;
            old.restorer = current_proc->sigactions[signum].restorer;
            pte_t *pte = walk(current_proc->pagetable, oldact_uaddr, 0);
            if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                uint64_t pa = PTE2PA(*pte) + (oldact_uaddr & 0xFFF);
                memcpy((void *)pa, &old, sizeof(old));
            }
        }
        /* Install new action if provided */
        if (act_uaddr) {
            struct { uint64_t handler; uint64_t mask; int flags; int pad; uint64_t restorer; } act;
            pte_t *pte = walk(current_proc->pagetable, act_uaddr, 0);
            if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                uint64_t pa = PTE2PA(*pte) + (act_uaddr & 0xFFF);
                memcpy(&act, (void *)pa, sizeof(act));
                current_proc->sigactions[signum].handler = act.handler;
                current_proc->sigactions[signum].mask = act.mask;
                current_proc->sigactions[signum].flags = act.flags;
                current_proc->sigactions[signum].restorer = act.restorer;
            }
        }
        tf->a0 = 0;
    }
}
```

**Step 4: Build and verify**

Run: `make clean && make`
Expected: Compiles clean.

**Step 5: Commit**

```bash
git add kernel/include/proc.h kernel/proc.c kernel/trap.c
git commit -m "ENH: Added SYS_rt_sigaction handler to store signal handlers"
```

---

## Task 3: Add `SYS_rt_sigprocmask` handler

**Files:**
- Modify: `kernel/trap.c`

**Step 1: Add handler**

Before the "Unknown syscall" catch-all:

```c
else if (num == 135) { /* SYS_rt_sigprocmask */
    int how = (int)tf->a0;
    uint64_t set_uaddr = tf->a1;
    uint64_t oldset_uaddr = tf->a2;
    /* a3 = sigsetsize, ignored */

    /* Return old mask if requested */
    if (oldset_uaddr) {
        uint64_t oldmask = current_proc->sigmask;
        pte_t *pte = walk(current_proc->pagetable, oldset_uaddr, 0);
        if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
            uint64_t pa = PTE2PA(*pte) + (oldset_uaddr & 0xFFF);
            memcpy((void *)pa, &oldmask, sizeof(oldmask));
        }
    }
    /* Apply new mask if provided */
    if (set_uaddr) {
        uint64_t newset = 0;
        pte_t *pte = walk(current_proc->pagetable, set_uaddr, 0);
        if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
            uint64_t pa = PTE2PA(*pte) + (set_uaddr & 0xFFF);
            memcpy(&newset, (void *)pa, sizeof(newset));
        }
        if (how == 0) /* SIG_BLOCK */
            current_proc->sigmask |= newset;
        else if (how == 1) /* SIG_UNBLOCK */
            current_proc->sigmask &= ~newset;
        else if (how == 2) /* SIG_SETMASK */
            current_proc->sigmask = newset;
    }
    tf->a0 = 0;
}
```

**Step 2: Build and verify**

Run: `make clean && make`
Expected: Compiles clean.

**Step 3: Commit**

```bash
git add kernel/trap.c
git commit -m "ENH: Added SYS_rt_sigprocmask handler for signal mask management"
```

---

## Task 4: Add `SYS_gettimeofday` handler

**Files:**
- Modify: `kernel/trap.c`

**Step 1: Add handler**

The kernel already tracks `ticks` (a global counter incremented by timer interrupts). Use it to populate `struct timeval`.

```c
else if (num == 169) { /* SYS_gettimeofday */
    uint64_t tv_uaddr = tf->a0;
    /* a1 = timezone pointer, ignored */
    if (tv_uaddr) {
        extern uint64_t ticks;
        struct { int64_t sec; int64_t usec; } tv;
        tv.sec = (int64_t)ticks;
        tv.usec = 0;
        pte_t *pte = walk(current_proc->pagetable, tv_uaddr, 0);
        if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
            uint64_t pa = PTE2PA(*pte) + (tv_uaddr & 0xFFF);
            memcpy((void *)pa, &tv, sizeof(tv));
        }
    }
    tf->a0 = 0;
}
```

**Step 2: Build and verify**

Run: `make clean && make`
Expected: Compiles clean.

**Step 3: Commit**

```bash
git add kernel/trap.c
git commit -m "ENH: Added SYS_gettimeofday handler using kernel tick counter"
```

---

## Task 5: Add `SYS_fcntl` handler

**Files:**
- Modify: `kernel/trap.c`

Minimal implementation for F_GETFL and F_SETFL.

**Step 1: Add handler**

```c
else if (num == 25) { /* SYS_fcntl */
    int fd = (int)tf->a0;
    int cmd = (int)tf->a1;

    if (fd < 0 || fd >= 16 || !current_proc->ofile[fd].used) {
        tf->a0 = (uint64_t)-9; /* -EBADF */
    } else if (cmd == 1) { /* F_GETFD */
        tf->a0 = 0;
    } else if (cmd == 2) { /* F_SETFD */
        tf->a0 = 0;
    } else if (cmd == 3) { /* F_GETFL */
        tf->a0 = 2; /* O_RDWR */
    } else if (cmd == 4) { /* F_SETFL */
        tf->a0 = 0;
    } else {
        tf->a0 = (uint64_t)-22; /* -EINVAL */
    }
}
```

**Step 2: Build, verify, commit**

```bash
make clean && make
git add kernel/trap.c
git commit -m "ENH: Added SYS_fcntl handler with basic F_GETFL/F_SETFL"
```

---

## Task 6: Add `SYS_faccessat` handler

**Files:**
- Modify: `kernel/trap.c`

Check whether a path exists in tarfs. For a read-only filesystem, only F_OK and R_OK make sense.

**Step 1: Add handler**

```c
else if (num == 48) { /* SYS_faccessat */
    /* a0=dirfd (ignored, use AT_FDCWD), a1=pathname, a2=mode, a3=flags */
    uint64_t path_uaddr = tf->a1;
    char path[256];
    /* copy path from user space */
    int plen = 0;
    while (plen < 255) {
        pte_t *pte = walk(current_proc->pagetable, path_uaddr + plen, 0);
        if (!pte || !(*pte & PTE_V)) break;
        uint64_t pa = PTE2PA(*pte) + ((path_uaddr + plen) & 0xFFF);
        char c = *(char *)pa;
        path[plen++] = c;
        if (c == '\0') break;
    }
    path[plen] = '\0';

    /* resolve and check tarfs */
    char resolved[256];
    /* Use resolve_path + tar_find_file — same pattern as SYS_openat */
    /* Simplified: just check if the file or directory exists */
    char tarpath[256];
    tarpath[0] = '.';
    tarpath[1] = '\0';

    /* Build absolute path */
    if (path[0] == '/') {
        strcpy(resolved, path);
    } else {
        strcpy(resolved, current_proc->cwd);
        if (resolved[strlen(resolved)-1] != '/')
            strcat(resolved, "/");
        strcat(resolved, path);
    }
    normalize_path(resolved);

    /* Convert to tarfs format: "./path" */
    if (resolved[0] == '/' && resolved[1] != '\0') {
        tarpath[0] = '.';
        strcpy(tarpath + 1, resolved);
    } else if (resolved[0] == '/' && resolved[1] == '\0') {
        strcpy(tarpath, "./");
    } else {
        tarpath[0] = '.';
        tarpath[1] = '/';
        strcpy(tarpath + 2, resolved);
    }

    extern char _tarfs_start[];
    extern char _tarfs_end[];
    void *found = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
    if (found || (resolved[0] == '/' && resolved[1] == '\0')) {
        tf->a0 = 0; /* file exists */
    } else {
        /* Try as directory (tarfs stores dirs with trailing /) */
        strcat(tarpath, "/");
        found = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
        tf->a0 = found ? 0 : (uint64_t)-2; /* -ENOENT */
    }
}
```

NOTE: The above is simplified. If `resolve_path()` and `to_tarfs_path()` are already static helpers in trap.c, reuse them. Adjust based on the actual helper signatures.

**Step 2: Build, verify, commit**

```bash
make clean && make
git add kernel/trap.c
git commit -m "ENH: Added SYS_faccessat handler to check tarfs file existence"
```

---

## Task 7: Add read-only FS stubs (unlinkat, linkat, renameat2, mkdirat, umask)

**Files:**
- Modify: `kernel/trap.c`

These all return `-EROFS` (30) since tarfs is read-only.

**Step 1: Add handlers**

```c
else if (num == 35) { /* SYS_unlinkat */
    tf->a0 = (uint64_t)-30; /* -EROFS */
}
else if (num == 37) { /* SYS_linkat */
    tf->a0 = (uint64_t)-30; /* -EROFS */
}
else if (num == 276) { /* SYS_renameat2 */
    tf->a0 = (uint64_t)-30; /* -EROFS */
}
else if (num == 34) { /* SYS_mkdirat */
    tf->a0 = (uint64_t)-30; /* -EROFS */
}
else if (num == 166) { /* SYS_umask */
    tf->a0 = 0022; /* return old umask, always 022 */
}
```

**Step 2: Build, verify, commit**

```bash
make clean && make
git add kernel/trap.c
git commit -m "ENH: Added EROFS stubs for unlinkat, linkat, renameat2, mkdirat"
```

---

## Task 8: Fix `execve` to pass environment variables

**Files:**
- Modify: `kernel/trap.c` (in the SYS_execve handler, around lines 833-958)

Currently `envp` (in `tf->a2`) is ignored. The fix: copy envp strings from the old process before switching page tables, then place them on the new stack after argv.

**Step 1: Copy envp strings alongside argv**

In the SYS_execve handler, after copying argv strings (around line 870), add envp copying:

```c
/* Copy envp strings from user space (before we destroy the old page table) */
char *kenvp[64];
int kenvpc = 0;
uint64_t envp_uaddr = tf->a2;
if (envp_uaddr) {
    for (int i = 0; i < 63; i++) {
        pte_t *pte = walk(current_proc->pagetable, envp_uaddr + i * 8, 0);
        if (!pte || !(*pte & PTE_V)) break;
        uint64_t pa = PTE2PA(*pte) + ((envp_uaddr + i * 8) & 0xFFF);
        uint64_t str_uaddr = *(uint64_t *)pa;
        if (str_uaddr == 0) break;

        /* Copy the string from user space */
        char buf[320];
        int len = 0;
        while (len < 319) {
            pte = walk(current_proc->pagetable, str_uaddr + len, 0);
            if (!pte || !(*pte & PTE_V)) break;
            pa = PTE2PA(*pte) + ((str_uaddr + len) & 0xFFF);
            buf[len] = *(char *)pa;
            if (buf[len] == '\0') break;
            len++;
        }
        buf[len] = '\0';

        /* Allocate kernel copy */
        kenvp[kenvpc] = kalloc();
        if (!kenvp[kenvpc]) break;
        memset(kenvp[kenvpc], 0, 4096);
        memcpy(kenvp[kenvpc], buf, len + 1);
        kenvpc++;
    }
}
```

**Step 2: Place envp on the new stack after argv**

After the argv pointer array is written to the new stack, continue with envp:

```c
/* envp pointers follow argv (after argv's NULL terminator) */
uint64_t envp_base = ptr_base + (kargc + 1) * 8; /* after argv[0..argc-1] + NULL */
for (int i = 0; i < kenvpc; i++) {
    /* Copy string to stack */
    int elen = strlen(kenvp[i]) + 1;
    sp -= elen;
    /* Copy string data to user stack page */
    /* ... (same pattern as argv string copying) ... */
    /* Write pointer */
    /* ... */
}
/* Write envp NULL terminator */
/* Set a2 = envp_base in trapframe */
newtf->a2 = envp_base;

/* Free temporary kernel copies */
for (int i = 0; i < kenvpc; i++)
    kfree(kenvp[i]);
```

NOTE: This task is complex. The exact implementation depends on the existing stack layout code in the SYS_execve handler. Read the full handler carefully and adapt the pattern used for argv copying. The key structure on the new stack should be:

```
[argc (8B)] [argv[0]..argv[argc-1] ptrs] [NULL] [envp[0]..envp[N-1] ptrs] [NULL] [string data...]
```

**Step 3: Build, test in QEMU, commit**

```bash
make clean && make && make qemu
# Verify: set PS1="test$ " && echo $PS1 → should print "test$ " if envp works
git add kernel/trap.c
git commit -m "ENH: Fixed execve to pass envp to child processes"
```
