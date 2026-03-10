# Signal And Alarm Fixes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix signal-mask bit numbering, restore the saved signal mask on `rt_sigreturn`, and make the `alarm_signal` test deterministic across machines.

**Architecture:** Standardize signal-set bit positions on `signum - 1` everywhere so libc and kernel interpret masks identically. Extend signal delivery to save the pre-handler mask beside the saved trapframe, then restore both in `rt_sigreturn`. Update `schedtest` to wait on elapsed wall time instead of a fixed CPU loop count.

**Tech Stack:** C, RISC-V kernel trap handling, SBUnix libc, QEMU, `make`, user-space regression tests

---

### Task 1: Align Signal Bit Numbering

**Files:**
- Modify: `kernel/trap.c`
- Modify: `kernel/tty.c`
- Modify: `kernel/include/proc.h`
- Test: `bin/sigpipetest/sigpipetest.c`
- Test: `bin/init/init.c`

**Step 1: Audit every kernel use of `sigpending` and `sigmask`**

Run: `rg -n "sigpending|sigmask|1UL << SIG|1UL << signum|1UL << sig" kernel`

Expected: every pending/mask mutation and delivery site is listed.

**Step 2: Add shared helpers for signal bit operations**

Implement small helpers/macros in `kernel/include/proc.h`:

```c
static inline uint64_t signal_bit(int signum) {
  return 1UL << (signum - 1);
}

static inline int signal_is_valid(int signum) {
  return signum > 0 && signum < 32;
}
```

**Step 3: Convert kernel users to the shared convention**

Update every site in `kernel/trap.c` and `kernel/tty.c` to use `signal_bit(signum)` instead of `1UL << signum` or `1UL << SIGFOO`.

**Step 4: Fix delivery scan**

In `deliver_signal()`, scan `signum` from `1` to `31` and test:

```c
if (deliverable & signal_bit(i)) {
```

Then clear pending with the same helper.

**Step 5: Build and run signal regression**

Run: `source tools/sourceme.sh && make`

Expected: build succeeds.

Run: `source tools/sourceme.sh && timeout 40 qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf -drive file=build/disk.img,format=raw,if=none,id=hd0 -device virtio-blk-pci-non-transitional,drive=hd0 -device virtio-gpu-pci -device virtio-net-pci-non-transitional,netdev=net0 -netdev user,id=net0 -nographic`

Expected: serial output contains `=== sigpipetest:` with all tests passing.

**Step 6: Commit**

```bash
git add kernel/include/proc.h kernel/trap.c kernel/tty.c
git commit -m "BF: Unified signal bit numbering"
```

### Task 2: Restore The Saved Signal Mask In `rt_sigreturn`

**Files:**
- Modify: `kernel/include/proc.h`
- Modify: `kernel/trap.c`
- Test: `bin/alarmtest/alarmtest.c`
- Test: `bin/sigpipetest/sigpipetest.c`

**Step 1: Define a user-stack signal frame**

Add a dedicated kernel-side struct in `kernel/include/proc.h`:

```c
struct signal_frame {
  uint64_t saved_sigmask;
  struct trapframe saved_tf;
};
```

**Step 2: Save both mask and trapframe during delivery**

In `deliver_signal()`, push a `struct signal_frame` to user stack, fill `saved_sigmask` from `p->sigmask`, and copy the whole frame out.

**Step 3: Restore both values in `sys_rt_sigreturn_handler()`**

Replace the trapframe-only restore with:

```c
struct signal_frame frame;
copyin(..., &frame, usp, sizeof(frame));
current_proc->sigmask = frame.saved_sigmask;
memcpy(tf, &frame.saved_tf, sizeof(frame.saved_tf));
```

**Step 4: Build and run signal regressions**

Run: `source tools/sourceme.sh && make`

Expected: build succeeds.

Run: `source tools/sourceme.sh && timeout 40 qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf -drive file=build/disk.img,format=raw,if=none,id=hd0 -device virtio-blk-pci-non-transitional,drive=hd0 -device virtio-gpu-pci -device virtio-net-pci-non-transitional,netdev=net0 -netdev user,id=net0 -nographic`

Expected: `sigpipetest` still passes and alarm-driven handlers continue returning to user mode cleanly.

**Step 5: Commit**

```bash
git add kernel/include/proc.h kernel/trap.c
git commit -m "BF: Restored saved masks after sigreturn"
```

### Task 3: Make `alarm_signal` Deterministic

**Files:**
- Modify: `bin/schedtest/schedtest.c`
- Test: `bin/schedtest/schedtest.c`

**Step 1: Replace the fixed spin with a real-time bounded wait**

Use `time(NULL)` or `clock_gettime()` to wait until either:
- `alarm_flag` becomes `1`, or
- a deadline around 3 seconds expires

Suggested structure:

```c
time_t deadline = time(NULL) + 3;
while (!alarm_flag && time(NULL) < deadline) {
    for (volatile int i = 0; i < 100000; i++);
}
```

**Step 2: Keep the assertion strict**

Leave:

```c
CHECK("alarm_signal", alarm_flag == 1);
```

The test should remain behavioral, only the waiting mechanism changes.

**Step 3: Build and run schedtest regression**

Run: `source tools/sourceme.sh && make`

Expected: build succeeds.

Run: `source tools/sourceme.sh && timeout 40 qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf -drive file=build/disk.img,format=raw,if=none,id=hd0 -device virtio-blk-pci-non-transitional,drive=hd0 -device virtio-gpu-pci -device virtio-net-pci-non-transitional,netdev=net0 -netdev user,id=net0 -nographic`

Expected: `=== schedtest:` shows `alarm_signal` passing.

**Step 4: Commit**

```bash
git add bin/schedtest/schedtest.c
git commit -m "BF: Stabilized alarm signal test"
```

### Task 4: Final Verification

**Files:**
- Test: `rootfs/etc/test.sh`
- Test: `tools/run_tests.sh`

**Step 1: Run the full suite**

Run: `source tools/sourceme.sh && bash tools/run_tests.sh`

Expected: all `[PASS]` lines, no `[FAIL]` lines.

**Step 2: Capture the result summary**

Record whether `schedtest`, `sigpipetest`, and `alarmtest` all pass in the final handoff.

**Step 3: Do not amend earlier commits**

If a follow-up fix is required, add a new commit rather than rewriting history.
