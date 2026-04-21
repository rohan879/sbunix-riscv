# Test Regression Review 01

Scope: code review only. I did not run a build or boot QEMU, so no git worktree was needed for this pass.

## Findings

### 1. `os-test: basic/unistd/fchownat` likely regressed in `22cf54a`

Severity: High

Why:

- Before `22cf54a`, `sys_fchownat_handler` was a pure single-user stub that always returned success (`22cf54a^:sbunix-skeleton/kernel/syscall_misc.c`, lines 489-492).
- `22cf54a` changed it to resolve the supplied dirfd/path pair and then require the target to exist via `ops->stat(...)` before returning success (`sbunix-skeleton/kernel/syscall_misc.c:524-545`, `sbunix-skeleton/kernel/pathres.c:146-158`).
- The os-test runner executes each test from its suite directory, not from the os-test source root. `run-os-test` does `chdir("/test/bin/os-test/<suite>")` and then `execv("./<rest>")` (`sbunix-skeleton/bin/run-os-test/run-os-test.c:274-301`).
- The affected test opens `".."` and then calls `fchownat(dir, "basic/unistd/fchownat.c", ...)` (`sbunix-skeleton/tests/thirdparty/sortix/os-test/basic/unistd/fchownat.c:10-15`).
- The guest image only stages compiled os-test binaries, manifests, `.expect` files, and `.out` seed files. It does not copy the os-test source tree into `/test/bin/os-test` or elsewhere (`sbunix-skeleton/Makefile:177-200`).

Impact:

- With the old stubbed handler, the test could pass even though `basic/unistd/fchownat.c` was not present in the guest.
- With the new existence-checking handler, the same call now resolves to a path that is absent from the staged guest tree, so the syscall can legitimately return `-ENOENT`, which matches a `PASS -> FAIL` transition.

Assessment:

- This looks like a real regression caused by the dirfd/path-resolution hardening in `22cf54a`, not by the vendored binary staging work.

### 2. `open-posix: conformance/interfaces/timer_getoverrun/1-1` is very likely an `ERR` because the harness times it out

Severity: High

Why:

- `run-open-posix-test` installs a hardcoded `alarm(5)` per test (`sbunix-skeleton/bin/run-open-posix-test/run-open-posix-test.c:66-79`).
- When that alarm fires, the harness kills the child with `SIGKILL` and keeps the raw abnormal exit result (`sbunix-skeleton/bin/run-open-posix-test/run-open-posix-test.c:181-205`).
- The result mapper only treats exit codes `0`, `1`, `2`, `4`, and `5` as structured POSIX outcomes; anything else becomes `RESULT_ERROR` (`sbunix-skeleton/bin/run-open-posix-test/run-open-posix-test.c:241-248`).
- Test `1-1` intentionally sleeps for `2*TIMERINTERVAL + TIMERVAL`, which is `8` seconds (`sbunix-skeleton/tests/thirdparty/open-posix-test-suite/posixtest/conformance/interfaces/timer_getoverrun/1-1.c:25-26`, `:100-114`).
- The 5-second watchdog was introduced by `e6c4acb` in the open-posix runner; the original runner waited normally and had no timeout path (`e6c4acb`, `e6c4acb^:sbunix-skeleton/bin/run-open-posix-test/run-open-posix-test.c:152-213`).

Impact:

- `1-1` cannot complete under the current runner. It will be killed by the harness before it reaches `timer_getoverrun()` and its final `PTS_PASS`/`PTS_FAIL` return.
- That converts a previously structured result into `ERR`.

Assessment:

- This is a harness regression, not evidence of a timer syscall regression.

### 3. `open-posix: conformance/interfaces/timer_getoverrun/2-1` is also forced to `ERR` by the same watchdog

Severity: High

Why:

- Test `2-1` sleeps for `VALUESEC + 2*INTERVALSEC + 1`, which is `12` seconds (`sbunix-skeleton/tests/thirdparty/open-posix-test-suite/posixtest/conformance/interfaces/timer_getoverrun/2-1.c:26-29`, `:75-89`).
- The same `alarm(5)` / `SIGKILL` / `RESULT_ERROR` path described above applies (`sbunix-skeleton/bin/run-open-posix-test/run-open-posix-test.c:66-79`, `:181-205`, `:241-248`).

Impact:

- `2-1` also cannot complete under the current harness and will be reported as `ERR`.

Assessment:

- The observed `FAIL -> ERR` change is best explained by the runner timeout masking the older functional result.

### 4. `timer_getoverrun/2-1` still appears functionally broken even after the harness issue

Severity: Medium

Why:

- `sig_set_pending()` coalesces standard signals and does not record a second pending instance when the same signal is already pending (`sbunix-skeleton/kernel/include/proc.h:319-333`).
- The timer interrupt path increments `overrun` only when it has to advance `next_expiry_ticks` multiple times within a single interrupt pass (`sbunix-skeleton/kernel/trap.c:296-312`).
- It does not increment `overrun` for the POSIX case where the timer fires again while its standard signal is already pending but no extra instance can be queued.

Impact:

- For `2-1`, blocked `SIGCONT` expirations at roughly 3s, 7s, and 11s should yield one pending signal plus two overruns.
- The current code path can leave `overrun` at `0` unless the interrupt handler itself wakes up after multiple whole periods have already elapsed.
- That matches the historical `FAIL` part of `FAIL -> ERR`.

Assessment:

- There are two separate issues here:
  1. The current `ERR` is a harness timeout artifact.
  2. The underlying timer overrun accounting for `2-1` still looks wrong by inspection.

## Third-Party Binary Integration Hypothesis

Short answer: I do not see a direct causal link.

What I checked:

- The third-party workload staging change in `7f7d4ea` copies vendored payloads into the normal rootfs and `/test` via `tools/stage-thirdparty.sh` (`sbunix-skeleton/Makefile:155-168`, `:260-266`).
- The third-party suites themselves are still built with the in-tree cross-compiler and SBUnix libc headers, not with the vendored TCC sysroot (`sbunix-skeleton/tools/os-test.mk:12-42`, `sbunix-skeleton/tools/open-posix-test.mk:12-42`).

Conclusion:

- `fchownat` points to the later dirfd/path-resolution change in `22cf54a`.
- The two `timer_getoverrun` cases point to the older open-posix watchdog change in `e6c4acb` plus a pre-existing kernel overrun-accounting limitation for `2-1`.
- The vendored binary integration work does not appear to be the direct source of these three regressions.

## Most Likely Root Causes

1. `basic/unistd/fchownat`
   Root cause: `22cf54a` changed `fchownat` from a success stub to an existence-checking implementation, while the guest still does not stage the os-test source file that this test references.

2. `timer_getoverrun/1-1`
   Root cause: `run-open-posix-test` now enforces a 5-second watchdog, but the test intentionally sleeps 8 seconds.

3. `timer_getoverrun/2-1`
   Immediate cause of `ERR`: same 5-second watchdog, but the test intentionally sleeps 12 seconds.
   Likely underlying functional cause of the earlier `FAIL`: timer overrun accounting does not track expirations that occur while the timer signal is already pending and coalesced.
