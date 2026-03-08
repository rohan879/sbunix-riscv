# Kernel Hardening & Refactoring Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix unsafe user pointer dereferences, refactor syscall dispatch to a table, and complete mmap/mprotect.

**Architecture:** Three sequential features: copyin/copyout validation functions → dispatch table extraction → mmap/mprotect completion.

**Tech Stack:** C, RISC-V (rv64imac), riscv64-unknown-elf-gcc, QEMU virt machine.

---

## Feature 1: copyin/copyout/copyinstr

### Task 1.1: Add copyin() to vm.c

**Files:**
- Modify: `kernel/vm.c` (after uvmcopyin at line 454)
- Modify: `kernel/include/vm.h` (add declaration)

**Step 1: Add copyin() function to vm.c**

Add after line 454 (after existing uvmcopyin):

```c
int copyin(pagetable_t pagetable, void *dst, uint64_t srcva, uint64_t len)
{
  char *d = (char *)dst;
  while (len > 0) {
    if (srcva >= MAXVA)
      return -1;
    int level;
    pte_t *pte = walk_super(pagetable, srcva, &level);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;

    uint64_t page_size = (level == 1) ? SUPERPAGE_SIZE : PAGE_SIZE;
    uint64_t va0 = (level == 1) ? SPGROUNDDOWN(srcva) : PGROUNDDOWN(srcva);
    uint64_t pa0 = PTE2PA(*pte) + (srcva - va0);
    uint64_t n = page_size - (srcva - va0);
    if (n > len)
      n = len;

    memcpy(d, (void *)pa0, n);

    len -= n;
    d += n;
    srcva = va0 + page_size;
  }
  return 0;
}
```

**Step 2: Add declaration to vm.h**

Add after the `uvmcopyin` declaration:

```c
int copyin(pagetable_t pagetable, void *dst, uint64_t srcva, uint64_t len);
```

**Step 3: Verify it compiles**

Run: `cd /home/adebayo/CSE506/sbunix-skeleton && source tools/sourceme.sh && make clean && make`
Expected: Compiles with no warnings/errors.

---

### Task 1.2: Rename uvmcopyin to copyout, add copyout declaration

**Files:**
- Modify: `kernel/vm.c` (rename function at line 432, add MAXVA check)
- Modify: `kernel/include/vm.h` (rename declaration)
- Modify: `kernel/exec.c` (update callsite at line 73)
- Modify: `kernel/trap.c` (update callsite at line 1340)

**Step 1: Rename uvmcopyin → copyout in vm.c**

At line 432, change:
```c
int uvmcopyin(pagetable_t pagetable, uint64_t dstva, char *src, uint64_t len) {
```
to:
```c
int copyout(pagetable_t pagetable, uint64_t dstva, void *src, uint64_t len) {
```

Add MAXVA check at the top of the while loop body (after `while (len > 0) {`):
```c
    if (dstva >= MAXVA)
      return -1;
```

Also add write permission check — after the existing PTE_V|PTE_U check:
```c
    if ((*pte & PTE_W) == 0 && (*pte & PTE_COW) == 0)
      return -1;
```

**Step 2: Update vm.h**

Replace `int uvmcopyin(...)` with:
```c
int copyout(pagetable_t pagetable, uint64_t dstva, void *src, uint64_t len);
```

**Step 3: Update exec.c callsite**

At line 73, change `uvmcopyin` to `copyout`.

**Step 4: Update trap.c callsite**

At line 1340, change `uvmcopyin` to `copyout`.

**Step 5: Verify it compiles**

Run: `make clean && make`
Expected: Compiles with no warnings/errors.

---

### Task 1.3: Add copyinstr() to vm.c

**Files:**
- Modify: `kernel/vm.c` (after copyin)
- Modify: `kernel/include/vm.h` (add declaration)

**Step 1: Add copyinstr() function**

```c
int copyinstr(pagetable_t pagetable, char *dst, uint64_t srcva, uint64_t max)
{
  int got_null = 0;
  while (max > 0) {
    if (srcva >= MAXVA)
      return -1;
    int level;
    pte_t *pte = walk_super(pagetable, srcva, &level);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;

    uint64_t page_size = (level == 1) ? SUPERPAGE_SIZE : PAGE_SIZE;
    uint64_t va0 = (level == 1) ? SPGROUNDDOWN(srcva) : PGROUNDDOWN(srcva);
    uint64_t pa0 = PTE2PA(*pte) + (srcva - va0);
    uint64_t n = page_size - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)pa0;
    for (uint64_t i = 0; i < n; i++) {
      *dst = p[i];
      if (p[i] == '\0') {
        got_null = 1;
        break;
      }
      dst++;
    }
    if (got_null)
      break;

    max -= n;
    srcva = va0 + page_size;
  }
  return got_null ? 0 : -1;
}
```

**Step 2: Add declaration to vm.h**

```c
int copyinstr(pagetable_t pagetable, char *dst, uint64_t srcva, uint64_t max);
```

**Step 3: Verify it compiles**

Run: `make clean && make`

---

### Task 1.4: Secure path-taking syscalls with copyinstr

**Files:**
- Modify: `kernel/trap.c`

These syscalls all follow the same pattern — cast tf->a1 (or tf->a0) to
`const char *path`, then call `resolve_path()`. Replace with `copyinstr`
to a kernel buffer.

**Syscalls to fix:**

1. **SYS_openat (line 1016)**: Replace `const char *path = (const char *)tf->a1;`
   with:
   ```c
   char kpath[256];
   if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
     tf->a0 = (uint64_t)-14; /* -EFAULT */
     goto syscall_done;
   }
   ```
   Then use `kpath` instead of `path` in `resolve_path()` call.

2. **SYS_chdir (line 1166)**: Same pattern, `tf->a0` → `kpath`.

3. **SYS_faccessat (line 1947)**: Same pattern, `tf->a1` → `kpath`.

4. **SYS_unlinkat (line 1970)**: Same pattern, `tf->a1` → `kpath`.

5. **SYS_mkdirat (line 2004)**: Same pattern, `tf->a1` → `kpath`.

6. **SYS_mknodat (line 2023)**: Same pattern, `tf->a1` → `kpath`.

**Step 1: Fix all 6 syscalls**

For each, replace the direct cast with the copyinstr pattern above.
Use the variable name `kpath` in all cases (existing code already declares
`char resolved[256]` separately).

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 1.5: Secure SYS_execve user pointer handling

**Files:**
- Modify: `kernel/trap.c` (lines 1186-1248)

**Step 1: Secure path, argv, and envp**

The current code at lines 1186-1248 already copies argv/envp strings to
kernel buffers (kstrbuf/kenvbuf), but it dereferences user pointers
directly to do so. Fix:

a) Path (line 1186): Replace `const char *path = (const char *)tf->a0;` +
   `strlen(path)` at line 1246 with:
   ```c
   char kpath[256];
   if (copyinstr(current_proc->pagetable, kpath, tf->a0, sizeof(kpath)) < 0) {
     tf->a0 = (uint64_t)-14;
     goto syscall_done;
   }
   ```

b) argv array (lines 1214-1228): Use `copyin` to read each argv pointer,
   then `copyinstr` to read each string:
   ```c
   uint64_t uargv = tf->a1;
   while (kargc < EXECVE_MAXARGS) {
     uint64_t uptr;
     if (copyin(current_proc->pagetable, &uptr, uargv + kargc * 8, 8) < 0)
       break;
     if (uptr == 0) break;
     char tmpstr[256];
     if (copyinstr(current_proc->pagetable, tmpstr, uptr, sizeof(tmpstr)) < 0)
       break;
     int slen = strlen(tmpstr) + 1;
     if (kstroff + slen > (int)sizeof(kstrbuf)) break;
     memcpy(kstrbuf + kstroff, tmpstr, slen);
     kargv[kargc] = kstrbuf + kstroff;
     kstroff += slen;
     kargc++;
   }
   ```

c) envp array (lines 1232-1244): Same pattern as argv.

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 1.6: Secure buffer-writing syscalls with copyout

**Files:**
- Modify: `kernel/trap.c`

**Syscalls to fix:**

1. **SYS_pipe2 (lines 1110, 1146-1147)**: Replace direct `fds[0] = rfd;`
   with:
   ```c
   int kfds[2] = { rfd, wfd };
   if (copyout(current_proc->pagetable, tf->a0, kfds, sizeof(kfds)) < 0) {
     // cleanup: close both fds
     tf->a0 = (uint64_t)-14;
     goto syscall_done;
   }
   ```

2. **SYS_getcwd (lines 1153, 1159)**: Replace direct `memcpy(buf, cwd, len+1)`
   with:
   ```c
   const char *cwd = current_proc->cwd;
   int len = strlen(cwd);
   if (copyout(current_proc->pagetable, tf->a0, (void *)cwd, len + 1) < 0) {
     tf->a0 = (uint64_t)-14;
     goto syscall_done;
   }
   tf->a0 = tf->a0; /* return buf pointer */
   ```

3. **SYS_fstat (lines 1468-1508)**: Build stat struct in kernel buffer first,
   then copyout:
   ```c
   uint8_t kstat[128];
   memset(kstat, 0, 128);
   // ... all the memcpy lines writing to kstat instead of st ...
   if (copyout(current_proc->pagetable, tf->a1, kstat, 128) < 0) {
     tf->a0 = (uint64_t)-14;
     goto syscall_done;
   }
   ```

4. **SYS_newfstatat (lines 1514-1527)**: Copy path with copyinstr, then
   build stat in kernel buffer and copyout.

5. **SYS_ps (lines 1733-1759)**: Build entire ps output in kernel buffer,
   then copyout at the end.

**Step 1: Fix all 5 syscalls**

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 1.7: Secure SYS_read and SYS_write buffer access

**Files:**
- Modify: `kernel/trap.c`

These are the most performance-sensitive syscalls. The user buffer `buf`
is used directly for I/O — pipe read/write and VFS read/write.

**SYS_write (lines 939, 957, 971)**:

For pipe writes (line 957: `buf[written + i]`), use copyin to read from
user buffer into a kernel staging buffer per chunk:
```c
char kchunk[64];
uint64_t chunk_len = (remaining > 64) ? 64 : remaining;
if (copyin(current_proc->pagetable, kchunk, (uint64_t)(buf + written), chunk_len) < 0) {
  tf->a0 = (uint64_t)-14;
  goto syscall_done;
}
// write kchunk to pipe
```

For VFS writes (line 971: `ops->write(f->private, buf, len, &f->offset)`),
the VFS backends already receive kernel-mapped memory (tarfs is embedded
in kernel, ext2 uses bcache). However the `buf` pointer itself is a user VA.
Copy to a kernel buffer first:
```c
char kbuf[512];
uint64_t chunk = (len > 512) ? 512 : len;
if (copyin(current_proc->pagetable, kbuf, (uint64_t)buf, chunk) < 0) { ... }
ret = f->ops->write(f->private, kbuf, chunk, &f->offset);
```

**SYS_read (lines 979, 993, 1008)**: Same pattern but reversed — VFS reads
into kernel buffer, then copyout to user buf. Pipe reads copy from pipe to
kernel buffer, then copyout.

**Step 1: Fix both syscalls with chunked copyin/copyout**

**Step 2: Verify it compiles and test basic I/O**

Run: `make clean && make`

---

### Task 1.8: Secure remaining walk()-based syscalls for consistency

**Files:**
- Modify: `kernel/trap.c`

These syscalls already use `walk()` inline to validate pointers. Convert
them to use copyin/copyout for consistency:

- SYS_clock_gettime (lines 1423-1426)
- SYS_gettimeofday (lines 1919-1922)
- SYS_rt_sigaction (lines 1831-1846)
- SYS_rt_sigprocmask (lines 1865-1877)
- SYS_rt_sigreturn (lines 1897-1900)
- SYS_nanosleep (lines 1616-1620)
- SYS_writev (lines 2066-2079)
- SYS_readv (lines 2120-2133)
- SYS_prlimit64 (lines 2180-2183)
- SYS_getrandom (lines 2198-2203)
- SYS_ioctl (lines 1779-1799)
- SYS_getdents64 (lines 1594-1600)

For each: replace the inline `walk()` + PTE check + `PTE2PA()` + memcpy
with the equivalent `copyin()` or `copyout()` call.

**Step 1: Convert all 12 syscalls**

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 1.9: Test copyin/copyout/copyinstr

**Step 1: Build and boot**

Run: `make clean && make && make qemu`

**Step 2: Test basic I/O**
```
echo hello
echo hello | cat
ls
ls /bin
cat /etc/rc
```

**Step 3: Test filesystem operations**
```
cd /mnt
mkdir testdir
ls
cd /
```

**Step 4: Test process operations**
```
ps
sleep 1
echo test1 | cat | cat
```

**Step 5: Test signals**
```
alarmtest
segvtest
```

Expected: All commands work identically to before. No regressions.

---

### Task 1.10: Commit Feature 1

```bash
git add kernel/vm.c kernel/include/vm.h kernel/exec.c kernel/trap.c
git commit -m "BF: Added copyin/copyout/copyinstr user pointer validation"
```

---

## Feature 2: Syscall Dispatch Table

### Task 2.1: Create syscall_table infrastructure

**Files:**
- Modify: `kernel/trap.c`

**Step 1: Add table definition and sentinel**

Near the top of trap.c (after the helper function declarations, before
trap_handler), add:

```c
#define NSYSCALLS 501
#define SYSCALL_NORETURN (-0x5BCALL)

static long (*syscall_table[NSYSCALLS])(struct trapframe *tf);

void syscall_init(void)
{
  /* Populated in Task 2.2-2.5 */
}
```

**Step 2: Add syscall_init declaration to a header**

Add to `kernel/include/proc.h` (or a new `kernel/include/trap.h`):
```c
void syscall_init(void);
```

**Step 3: Call syscall_init from boot**

In `kernel/kernel.c`, add `syscall_init()` call before `userinit()`.

**Step 4: Verify it compiles**

Run: `make clean && make`

---

### Task 2.2: Extract process/memory syscalls into static functions

**Files:**
- Modify: `kernel/trap.c`

Extract these syscall branches into static functions above trap_handler:

```c
static long sys_brk(struct trapframe *tf) { /* lines 911-935 */ }
static long sys_clone(struct trapframe *tf) { /* line 1180-1183, calls sys_clone helper */ }
static long sys_execve(struct trapframe *tf) { /* lines 1185-1355 */ }
static long sys_wait4(struct trapframe *tf) { /* lines 1357-1361 */ }
static long sys_exit(struct trapframe *tf) { /* lines 1629-1661 */ }
static long sys_getpid(struct trapframe *tf) { return current_proc->pid; }
static long sys_getppid(struct trapframe *tf) { ... }
static long sys_getuid(struct trapframe *tf) { return 0; }
static long sys_getgid(struct trapframe *tf) { return 0; }
static long sys_geteuid(struct trapframe *tf) { return 0; }
static long sys_getegid(struct trapframe *tf) { return 0; }
static long sys_kill(struct trapframe *tf) { /* lines 1378-1411 */ }
static long sys_tgkill(struct trapframe *tf) { /* lines 2281-2303 */ }
static long sys_set_tid_address(struct trapframe *tf) { return current_proc->pid; }
static long sys_mmap(struct trapframe *tf) { /* lines 1663-1705 */ }
static long sys_munmap(struct trapframe *tf) { /* lines 1707-1723 */ }
static long sys_mprotect(struct trapframe *tf) { /* lines 1725-1727 */ }
static long sys_madvise(struct trapframe *tf) { return 0; }
```

Register in syscall_init():
```c
syscall_table[SYS_brk] = sys_brk;
syscall_table[SYS_clone] = sys_clone;
// ... etc
```

For sys_clone and sys_exit which don't return normally, return SYSCALL_NORETURN.

**Step 1: Extract all process/memory syscalls**

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 2.3: Extract file I/O syscalls into static functions

**Files:**
- Modify: `kernel/trap.c`

```c
static long sys_write(struct trapframe *tf) { /* lines 937-975 */ }
static long sys_read(struct trapframe *tf) { /* lines 977-1012 */ }
static long sys_openat(struct trapframe *tf) { /* lines 1014-1070 */ }
static long sys_close(struct trapframe *tf) { /* lines 1072-1079 */ }
static long sys_dup3(struct trapframe *tf) { /* lines 1081-1107 */ }
static long sys_pipe2(struct trapframe *tf) { /* lines 1109-1150 */ }
static long sys_getcwd(struct trapframe *tf) { /* lines 1152-1163 */ }
static long sys_chdir(struct trapframe *tf) { /* lines 1165-1178 */ }
static long sys_lseek(struct trapframe *tf) { /* lines 1435-1463 */ }
static long sys_fstat(struct trapframe *tf) { /* lines 1465-1510 */ }
static long sys_newfstatat(struct trapframe *tf) { /* lines 1512-1531 */ }
static long sys_getdents64(struct trapframe *tf) { /* lines 1533-1607 */ }
static long sys_readv(struct trapframe *tf) { /* lines 2110-2162 */ }
static long sys_writev(struct trapframe *tf) { /* lines 2056-2108 */ }
static long sys_fcntl(struct trapframe *tf) { /* lines 1927-1943 */ }
static long sys_faccessat(struct trapframe *tf) { /* lines 1945-1966 */ }
static long sys_unlinkat(struct trapframe *tf) { /* lines 1968-1995 */ }
static long sys_linkat(struct trapframe *tf) { return -30; }
static long sys_renameat2(struct trapframe *tf) { return -30; }
static long sys_mkdirat(struct trapframe *tf) { /* lines 2002-2020 */ }
static long sys_mknodat(struct trapframe *tf) { /* lines 2021-2040 */ }
static long sys_readlinkat(struct trapframe *tf) { return -2; }
static long sys_ftruncate(struct trapframe *tf) { return -30; }
```

Register all in syscall_init().

**Step 1: Extract all file I/O syscalls**

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 2.4: Extract signal/time/misc syscalls into static functions

**Files:**
- Modify: `kernel/trap.c`

```c
static long sys_rt_sigaction(struct trapframe *tf) { /* lines 1810-1854 */ }
static long sys_rt_sigprocmask(struct trapframe *tf) { /* lines 1856-1886 */ }
static long sys_rt_sigreturn(struct trapframe *tf) { /* lines 1888-1908 */ }
static long sys_alarm(struct trapframe *tf) { /* lines 2260-2271 */ }
static long sys_clock_gettime(struct trapframe *tf) { /* lines 1413-1433 */ }
static long sys_gettimeofday(struct trapframe *tf) { /* lines 1910-1925 */ }
static long sys_nanosleep(struct trapframe *tf) { /* lines 1609-1627 */ }
static long sys_ioctl(struct trapframe *tf) { /* lines 1765-1808 */ }
static long sys_ps(struct trapframe *tf) { /* lines 1729-1763 */ }
static long sys_setpgid(struct trapframe *tf) { /* lines 2213-2225 */ }
static long sys_getpgid(struct trapframe *tf) { /* lines 2232-2244 */ }
static long sys_setsid(struct trapframe *tf) { /* lines 2227-2230 */ }
static long sys_getsid(struct trapframe *tf) { /* lines 2246-2258 */ }
static long sys_umask(struct trapframe *tf) { return 0; }
static long sys_prlimit64(struct trapframe *tf) { /* lines 2164-2186 */ }
static long sys_getrandom(struct trapframe *tf) { /* lines 2188-2211 */ }
```

Register all in syscall_init().

**Step 1: Extract all remaining syscalls**

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 2.5: Replace if-else chain with table dispatch

**Files:**
- Modify: `kernel/trap.c`

**Step 1: Replace the entire if-else chain**

Delete lines 908-2303 (the entire if-else chain) and replace with:

```c
  long sysnum = (long)tf->a7;
  if (sysnum >= 0 && sysnum < NSYSCALLS && syscall_table[sysnum]) {
    long ret = syscall_table[sysnum](tf);
    if (ret != SYSCALL_NORETURN)
      tf->a0 = (uint64_t)ret;
  } else {
    printk("Unknown syscall %ld from pid %d\n", sysnum, current_proc->pid);
    tf->a0 = (uint64_t)-38; /* -ENOSYS */
  }
```

Keep the `syscall_done:` label for signal delivery after syscall.

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 2.6: Test dispatch table

**Step 1: Build and boot**

Run: `make clean && make && make qemu`

**Step 2: Test all major subsystems**
```
echo hello
echo hello | cat
ls
ls /bin
cat /etc/rc
cd /mnt
mkdir test2
ls
cd /
ps
sleep 1
alarmtest
segvtest
```

Expected: All commands work identically. No regressions.

---

### Task 2.7: Commit Feature 2

```bash
git add kernel/trap.c kernel/kernel.c kernel/include/proc.h
git commit -m "RF: Refactored syscall dispatch to function pointer table"
```

---

## Feature 3: Complete mmap/munmap/mprotect

### Task 3.1: Fix uvmalloc to accept full permission flags

**Files:**
- Modify: `kernel/vm.c` (line 172, 186)
- Modify: `kernel/include/vm.h` (update declaration)

**Step 1: Change uvmalloc signature**

Current (line 172):
```c
uint64_t uvmalloc(pagetable_t pagetable, uint64_t start_va, uint64_t end_va, int xperm)
```

Change `xperm` to `perm` — the caller now passes the FULL set of PTE flags
(except PTE_V and PTE_U which are always added):

```c
uint64_t uvmalloc(pagetable_t pagetable, uint64_t start_va, uint64_t end_va, int perm)
```

At line 186, change:
```c
if(mappages(pagetable, a, PAGE_SIZE, (uint64_t)mem, PTE_R|PTE_U|xperm) != 0){
```
to:
```c
if(mappages(pagetable, a, PAGE_SIZE, (uint64_t)mem, perm|PTE_U|PTE_V) != 0){
```

**Step 2: Update all existing uvmalloc callers**

Search for all uvmalloc calls in the codebase and update the last argument:
- In trap.c (brk handler): was `PTE_W` → change to `PTE_R|PTE_W`
- In trap.c (stack auto-grow): was `PTE_W` → change to `PTE_R|PTE_W`
- In trap.c (demand paging for VMA): already uses VMA prot, convert prot to PTE flags
- In exec.c: was `PTE_W|PTE_X` → keep as `PTE_R|PTE_W|PTE_X`

**Step 3: Update vm.h declaration**

**Step 4: Verify it compiles**

Run: `make clean && make`

---

### Task 3.2: Fix mmap to honor prot argument

**Files:**
- Modify: `kernel/trap.c` (sys_mmap function)

**Step 1: Add prot-to-PTE translation in sys_mmap**

Replace the hardcoded `PTE_W` in the uvmalloc call with:

```c
int pte_flags = PTE_R; /* always readable at minimum */
if (prot & PROT_READ)  pte_flags |= PTE_R;
if (prot & PROT_WRITE) pte_flags |= PTE_W;
if (prot & PROT_EXEC)  pte_flags |= PTE_X;

if (uvmalloc(current_proc->pagetable, va, va + len, pte_flags) == 0) {
  tf->a0 = (uint64_t)-1;
  goto syscall_done;
}
```

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 3.3: Fix munmap for multi-VMA spans

**Files:**
- Modify: `kernel/trap.c` (sys_munmap function)

**Step 1: Replace single vma_find with loop**

```c
static long sys_munmap(struct trapframe *tf)
{
  uint64_t addr = tf->a0 & ~(uint64_t)(PAGE_SIZE - 1);
  uint64_t len  = tf->a1;
  len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

  if (addr >= MAXVA || addr + len > MAXVA)
    return -22; /* -EINVAL */

  /* Unmap pages — uvmunmap handles unmapped gaps gracefully */
  uvmunmap(current_proc->pagetable, addr, len);
  sfence_vma();

  /* Remove VMA tracking (handles splits automatically) */
  vma_remove(current_proc, addr, addr + len);

  return 0;
}
```

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 3.4: Implement mprotect

**Files:**
- Modify: `kernel/trap.c` (sys_mprotect function)
- Modify: `kernel/include/vm.h` (if needed)

**Step 1: Replace stub with full implementation**

```c
static long sys_mprotect(struct trapframe *tf)
{
  uint64_t addr = tf->a0;
  uint64_t len  = tf->a1;
  int prot      = (int)tf->a2;

  /* Validate alignment */
  if (addr & (PAGE_SIZE - 1))
    return -22; /* -EINVAL */
  len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
  if (len == 0)
    return 0;
  if (addr + len > MAXVA)
    return -22;

  /* Build new PTE permission bits */
  int new_flags = 0;
  if (prot & PROT_READ)  new_flags |= PTE_R;
  if (prot & PROT_WRITE) new_flags |= PTE_W;
  if (prot & PROT_EXEC)  new_flags |= PTE_X;

  /* Walk each page and update PTE flags */
  for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
    int level;
    pte_t *pte = walk_super(current_proc->pagetable, va, &level);

    /* Split super page if needed */
    if (pte && (*pte & PTE_V) && level == 1) {
      superpage_split(current_proc->pagetable, va & ~SUPERPAGE_MASK);
      pte = walk(current_proc->pagetable, va, 0);
    }

    if (!pte || !(*pte & PTE_V))
      continue;

    /* Clear old R/W/X, set new ones, keep U and other flags */
    *pte = (*pte & ~(PTE_R | PTE_W | PTE_X)) | new_flags;
  }

  /* Update VMA tracking: remove old region, re-add with new prot */
  struct vm_area *v = vma_find(current_proc, addr);
  int old_flags = v ? v->flags : 0;
  vma_remove(current_proc, addr, addr + len);
  vma_add(current_proc, addr, addr + len, prot, old_flags);

  sfence_vma();
  return 0;
}
```

**Step 2: Verify it compiles**

Run: `make clean && make`

---

### Task 3.5: Test mmap/munmap/mprotect

**Step 1: Build and boot**

Run: `make clean && make && make qemu`

**Step 2: Test basic functionality**
```
echo hello
ls
cat /etc/rc
ps
```

**Step 3: Test memory-intensive operations**
```
echo hello | cat | cat
sleep 1
alarmtest
segvtest
```

Expected: All commands work. mmap-based allocations (if any userspace uses
them) respect protection flags. mprotect changes are effective.

---

### Task 3.6: Commit Feature 3

```bash
git add kernel/trap.c kernel/vm.c kernel/include/vm.h
git commit -m "ENH: Completed mmap prot handling and mprotect implementation"
```

---

## Final Verification

### Task 4.1: Full regression test

Boot QEMU and run every available command:

```
echo hello world
echo test | cat
echo multi | cat | cat
ls
ls /bin
ls /dev
ls /proc
ls /sys
cat /etc/rc
cat /proc/mounts
cd /bin
pwd
cd /
cd /mnt
mkdir regression_test
ls
cd /
ps
sleep 1
kill <some_pid>    (if background process running)
alarmtest
segvtest
exit
```

Expected: All commands produce correct output. No crashes, no panics.
