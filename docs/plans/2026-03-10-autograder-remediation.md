# Autograder Remediation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remediate the confirmed auto-grader failures around `/bin/sh` compliance and `sleep_returns`, plus the closest shell-path regression that is likely to be graded next.

**Architecture:** Keep the fixes minimal and separable. One task makes `/bin/sh` a real executable in the packed rootfs and converts shipped scripts to `#!/bin/sh`; one task fixes timed sleep bookkeeping so `sleep(1)` and related timeout paths use consistent time units; one small adjacent task aligns the exported passwd shell path with the new standard shell location.

**Tech Stack:** GNU Make, C, embedded tarfs rootfs, QEMU `virt`, RISC-V cross toolchain, manual/tmux-based guest verification.

---

### Task 1: Fix `/bin/sh` rootfs compliance

**Files:**
- Modify: `Makefile`
- Modify: `rootfs/etc/rc`
- Modify: `rootfs/etc/test.sh`

**Reproduction commands:**

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
make build/kernel.elf
tar tf build/rootfs.tar | rg '^\./bin/(sbush|sh)$' || true
tar xf build/rootfs.tar -O ./etc/rc | sed -n '1,3p'
tar xf build/rootfs.tar -O ./etc/test.sh | sed -n '1,2p'
```

**Expected failing behavior before the fix:**
- Tar listing contains `./bin/sbush` but not `./bin/sh`.
- `rc` starts with `#!/bin/sbush`.
- `test.sh` starts with `#!/bin/sbush`.

**Implementation target:**
- Update the build so `build/rootfs/bin/sh` is created from the built `sbush` binary before tar packing.
- Convert shipped rootfs scripts to `#!/bin/sh`.
- Prefer a duplicated binary over a symlink because current tarfs/exec paths do not resolve tar symlink entries as executables.

**Verification commands:**

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
make build/kernel.elf
tar tf build/rootfs.tar | rg '^\./bin/(sbush|sh)$'
tar xf build/rootfs.tar -O ./etc/rc | sed -n '1p'
tar xf build/rootfs.tar -O ./etc/test.sh | sed -n '1p'
```

**Commit message to use:**

```text
BF: Added Standard /bin/sh rootfs entry
```

### Task 2: Verify script execution through `/bin/sh`

**Files:**
- No additional source changes expected beyond Task 1

**Reproduction commands:**

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
cp -f build/disk.img /tmp/sbunix-shcheck.img
tmux kill-session -t sbx_shcheck 2>/dev/null || true
tmux new-session -d -s sbx_shcheck -x 220 -y 60 \
  'cd /home/adebayo/CSE506/sbunix-skeleton && source tools/sourceme.sh && \
   qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf \
   -drive file=/tmp/sbunix-shcheck.img,format=raw,if=none,id=hd0 \
   -device virtio-blk-pci-non-transitional,drive=hd0 \
   -device virtio-gpu-pci \
   -device virtio-net-pci-non-transitional,netdev=net0 \
   -netdev user,id=net0 -nographic'
sleep 8
tmux send-keys -t sbx_shcheck '/bin/sh /etc/test.sh' C-m
sleep 3
tmux capture-pane -t sbx_shcheck -p -S -120 | tail -40
```

**Expected failing behavior before the fix:**
- Before Task 1, `/bin/sh /etc/test.sh` prints `sbush: /bin/sh: exec failed`.

**Implementation target:**
- No extra code if Task 1 is sufficient.
- If verification still fails, inspect `kernel/trap.c:sys_execve_handler`, `kernel/exec.c`, and tarfs open/stat behavior for the exact resolution failure.

**Verification commands:**

```bash
tmux send-keys -t sbx_shcheck '/etc/test.sh' C-m
sleep 3
tmux capture-pane -t sbx_shcheck -p -S -160 | tail -60
tmux kill-session -t sbx_shcheck
```

**Expected passing behavior after the fix:**
- `/bin/sh /etc/test.sh` runs the test script.
- `/etc/test.sh` also runs directly via shebang resolution.

**Commit message to use:**

```text
No commit if Task 1 already covered this verification
```

### Task 3: Fix timed sleep bookkeeping for `sleep_returns`

**Files:**
- Modify: `kernel/trap.c`
- Modify: `kernel/include/proc.h`
- Inspect: `libc/unistd.c`
- Inspect: `libc/time.c`

**Reproduction commands:**

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
make build/kernel.elf build/disk.img
cp -f build/disk.img /tmp/sbunix-schedtest.img
tmux kill-session -t sbx_schedtest 2>/dev/null || true
tmux new-session -d -s sbx_schedtest -x 220 -y 60 \
  'cd /home/adebayo/CSE506/sbunix-skeleton && source tools/sourceme.sh && \
   qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf \
   -drive file=/tmp/sbunix-schedtest.img,format=raw,if=none,id=hd0 \
   -device virtio-blk-pci-non-transitional,drive=hd0 \
   -device virtio-gpu-pci \
   -device virtio-net-pci-non-transitional,netdev=net0 \
   -netdev user,id=net0 -nographic'
sleep 8
tmux send-keys -t sbx_schedtest 'schedtest' C-m
sleep 6
tmux capture-pane -t sbx_schedtest -p -S -220 | tail -80
```

**Expected failing behavior before the fix:**
- `schedtest` reports `[FAIL] sleep_returns`.
- Current code stores `sleep_until` in inconsistent units:
  - `sys_nanosleep_handler` uses `ticks + secs`
  - `sys_clock_nanosleep_handler`, `ppoll`, and `pselect6` use `ticks + ns / 100`
  - wakeup compares `ticks >= p->sleep_until`

**Implementation target:**
- Make timed sleeps use one unit consistently.
- Recommended minimal fix: treat `sleep_until` as an absolute `r_time()` machine-time deadline and wake timed sleepers when `r_time() >= p->sleep_until`.
- Update `sys_nanosleep_handler`, `sys_clock_nanosleep_handler`, `ppoll`, and `pselect6` to compute deadlines in machine-time ticks.
- Preserve existing `alarm_ticks` behavior; do not couple it to the timed-sleep deadline change.

**Verification commands:**

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
make
cp -f build/disk.img /tmp/sbunix-schedtest.img
tmux kill-session -t sbx_schedtest 2>/dev/null || true
tmux new-session -d -s sbx_schedtest -x 220 -y 60 \
  'cd /home/adebayo/CSE506/sbunix-skeleton && source tools/sourceme.sh && \
   qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf \
   -drive file=/tmp/sbunix-schedtest.img,format=raw,if=none,id=hd0 \
   -device virtio-blk-pci-non-transitional,drive=hd0 \
   -device virtio-gpu-pci \
   -device virtio-net-pci-non-transitional,netdev=net0 \
   -netdev user,id=net0 -nographic'
sleep 8
tmux send-keys -t sbx_schedtest 'schedtest' C-m
sleep 6
tmux capture-pane -t sbx_schedtest -p -S -220 | tail -80
tmux kill-session -t sbx_schedtest
```

**Expected passing behavior after the fix:**
- `schedtest` reports `[PASS] sleep_returns`.
- Existing `alarm_signal` in the same test still passes.

**Commit message to use:**

```text
BF: Fixed Timed sleep deadline bookkeeping
```

### Task 4: Align exported passwd shell path

**Files:**
- Modify: `libc/pwd.c`

**Reproduction commands:**

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
sed -n '1,40p' libc/pwd.c
```

**Expected failing behavior before the fix:**
- The static root passwd entry exports `/bin/sbush` as the login shell even after `/bin/sh` becomes the standard shell path.

**Implementation target:**
- Change the exported root shell to `/bin/sh`.
- Keep the shell implementation itself as `sbush`; only the public standard path changes.

**Verification commands:**

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
make build/rootfs/bin/pwd
riscv64-unknown-elf-strings build/rootfs/bin/pwd build/libc.a | rg '/bin/sh'
```

**Commit message to use:**

```text
BF: Updated Root shell path export
```

### Task 5: Broader regression sweep after integration

**Files:**
- No planned source edits

**Verification commands:**

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
make
bash tools/run_tests.sh
```

**Expected behavior:**
- Build succeeds with `-Wall -Werror`.
- `tools/run_tests.sh` finishes without failed tests, including `sleep_returns`.

**Commit message to use:**

```text
No commit unless an additional regression fix is required
```

## Parallelization Notes

- Task 1 and Task 3 can run in parallel in separate worktrees.
- Task 4 can run in parallel with either Task 1 or Task 3, but it is tiny and can be batched with Task 1 because both are shell-path related.
- Task 2 depends on Task 1.
- Task 5 depends on integration of all committed fixes.

## Risks And Unknowns

- Tarfs does not currently resolve tar symlink entries as executable files, so a source symlink for `/bin/sh` is risky.
- Timed sleep code is shared by `nanosleep`, `clock_nanosleep`, `ppoll`, and `pselect6`; a partial fix only in `sleep()` is likely to leave near-term grader failures behind.
- `tools/run_tests.sh` uses a long full-system boot path, so targeted tmux/QEMU checks are faster for task-level verification before the final regression sweep.
