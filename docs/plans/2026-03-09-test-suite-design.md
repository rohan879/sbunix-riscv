# Comprehensive Test Suite — Design Document

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a bug-finding test suite (~60 tests) across 5 grading categories, with automated host-side runner.

**Architecture:** Three-layer testing: C test programs (kernel-level), shell test script (integration), host-side QEMU runner. Tests ordered simple→complex. Each test prints `[PASS] name` / `[FAIL] name` to serial.

**Tech Stack:** C (cross-compiled with riscv64-unknown-elf-gcc), shell scripts (sbush), QEMU runner (bash)

---

## Test Architecture

### Layer 1: C Test Programs (`bin/*test/`)
Standalone binaries testing kernel features directly. Each uses a shared `CHECK(label, cond)` macro. No shell dependency — init can exec them directly.

### Layer 2: Shell Test Script (`rootfs/etc/test.sh`)
Exercises shell features, binary utilities, integration scenarios. Run by sbush.

### Layer 3: Host-Side Runner (`tools/run_tests.sh`)
Boots QEMU with timeout, captures serial output, counts `[PASS]`/`[FAIL]`, prints summary. Invoked via `make test`.

### Boot Flow
1. `init` runs `/etc/rc` which sources `/etc/test.sh`
2. `test.sh` runs C test binaries first, then shell-level tests
3. After all tests, init spawns shell as usual

---

## Critical Bug Fixes (Phase A — Before Tests)

### Bug 1: No SIGPIPE on closed pipe (HIGH)
- **Problem:** Pipe write with closed read end blocks forever
- **Fix:** In pipe write path, check `read_open == 0`, deliver SIGPIPE, return -EPIPE
- **Location:** `kernel/trap.c` pipe write handler

### Bug 2: WNOHANG not working in waitpid (MEDIUM)
- **Problem:** wait4 with WNOHANG may block instead of returning 0
- **Fix:** When WNOHANG set and no zombie child, return 0 immediately
- **Location:** `kernel/trap.c` sys_wait4_handler

### Bug 3: 16-page stack auto-grow limit (LOW-MEDIUM)
- **Problem:** Stack only grows 64KB beyond initial allocation
- **Fix:** Increase limit to 256 pages (1MB) or use RLIMIT_STACK
- **Location:** `kernel/trap.c` page fault handler

---

## Test Categories

### Category 1: Cooperative Multi-tasking (`bin/proctest/`)
- fork returns 0 to child, >0 to parent
- fork+exec produces correct output
- waitpid returns correct PID and exit status
- WIFEXITED, WEXITSTATUS work correctly
- waitpid(-1) reaps any child
- exec non-existent path returns -1, errno==ENOENT
- getpid() != getppid()
- Fork child writes to variable, parent sees old value (address space isolation)
- Fork 5 children, wait all
- Orphan reparenting (ppid becomes 1)

### Category 2: Preemptive Multi-tasking (`bin/schedtest/`)
- Two spin-loop children both complete (no starvation)
- Parent and child both print without yield()
- time() returns >1000000000 (epoch)
- Two successive time() calls: t2 >= t1
- sleep(1) returns after ~1 second
- alarm(1) + busy wait: SIGALRM arrives within 2s
- Three concurrent children all scheduled

### Category 3: Signals and Pipes (`bin/sigpipetest/`)
- kill(child, SIGINT) terminates child
- WIFSIGNALED/WTERMSIG for killed child
- sigaction + raise() invokes handler
- SIGALRM handler with alarm()
- SIGSEGV on null deref (child dies or handler runs)
- SIG_IGN works
- SIGKILL unconditionally terminates
- pipe write/read basic
- pipe EOF on close write end
- pipe with fork (cross-process)
- pipe capacity (4096 bytes)
- dup2 redirect

### Category 4: On-disk Filesystem (`bin/fstest/`)
- create + write + close + reopen + read matches
- mkdir + stat shows S_ISDIR
- unlink removes file
- Multi-block write (>4096 bytes)
- lseek to middle
- O_APPEND mode
- stat st_size matches
- O_TRUNC resets size
- Nested directories
- rmdir empty directory
- open non-existent without O_CREAT returns ENOENT

### Category 5: Swap and Paging (`bin/swaptest/`)
- mmap anon page write/read
- mmap multi-page fill/verify
- COW fork isolation
- mprotect PROT_READ enforced
- munmap frees region
- Deep recursion (stack auto-grow)
- Memory pressure (swap eviction)
- malloc large allocation

### Category 6: Shell & Utilities Integration (`rootfs/etc/test.sh`)
- echo, cat, ls basic usage
- Shell pipes (single and multi-stage)
- Script creation on /mnt and execution
- mkdir binary
- cp binary to /mnt and exec from disk
- sleep completes

---

## Commit Convention

- Prefix: `ENH:` | `BF:` | `RF:` | `DOC:` | `CHORE:` | `TEST:`
- Capitalize first letter after colon
- 8-12 words max, past tense
- No Co-Authored-By line

## Verify-After-Commit Convention

After every commit:
1. `make` — compiles without errors
2. `make qemu` / `make test` — feature works
3. Check serial output for relevant `[PASS]`/`[FAIL]`
4. If test fails → fix, commit fix, re-verify

---

## Execution Plan

- **Phase A** (sequential): Fix 3 critical bugs → 3 commits, each verified
- **Phase B** (sequential): Build test infrastructure → ~3 commits, each verified
- **Phase C** (5 parallel agents in worktrees): Build test programs, fix bugs found → multiple commits per agent
- **Phase D** (sequential): Full integration run → fix stragglers
