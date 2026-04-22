# RV64IMAC Third-Party Binary Enablement and Remediation Plan

## Brief summary
- `sbunix-skeleton/docs/3rd-party-install.md` is the canonical source for original third-party build instructions. Use it as the authoritative rebuild/retarget reference instead of inventing a new external recipe.
- `make submit` survival is **out of scope for now**. This pass only needs deterministic local/worktree staging into the guest images used for validation.
- `/proc` is now an explicit workstream. The tree already has `kernel/procfs.c`, but the plan treats it as only partially proven because the exact external `/proc` row was never run.
- Do **not** plan to rewrite the current TCC binary’s baked-in paths to `~/CSE506/sbunix-skeleton/thirdparty/...`. That is not a sane target: `~` is shell expansion, not a runtime path resolver inside TCC; the string lengths differ; and the host repo path does not exist inside SBUnix. Stick to the original retarget/rebuild plan from `docs/3rd-party-install.md`.
- For any change in an existing file, add a short why-comment next to the changed logic so the reason survives review and later debugging. Every commit must follow the repo’s commit-message directives and capture the same rationale cleanly (e.g. ENH:, OPS, BLD, MNT, etc.).

## 1. Global diagnosis summary
- The current third-party story is still fraudulent: the assets exist in `thirdparty/`, but the build/install path is stale and incomplete.
- The binary blockers are not dynamic linking. The vendored third-party ELFs currently staged under `thirdparty/bin` are static RV64 soft-float binaries with no `PT_INTERP`.
- The real blockers are staging/layout, `clone`/`exec` semantics, TCC private-dir/sysroot visibility, BusyBox applet coverage, `/proc` exact-row validation, and unresolved POSIX seams still visible in `os-test` and `open-posix`.
- `plan-03.md` deferred the real third-party work. The replacement plan must not repeat that dodge.
- Before Wave 1 starts, produce a `plan-03.md` audit table that explicitly lists what was partially implemented, what was never completed, which assumptions were wrong or incomplete, what should be preserved, and what should be discarded or revised.
- Before Wave 1 starts, correlate `evaluation-summary.md`, `test-expectations.md`, `test-findings-19.md`, and `test-findings-19-artifacts/` into a ranked evidence matrix so every high-priority work item is tied to an observed failure, reproduction artifact, or stated requirement instead of asserted from memory.

## 2. Ranked issue clusters by priority
1. Third-party staging and runtime layout
- Subsystem: build/image/runtime layout.
- Problem: binaries and support assets are present but not staged deterministically into the guest.
- Inspect: `sbunix-skeleton/Makefile`, `sbunix-skeleton/thirdparty/Makefile`, `sbunix-skeleton/rootfs`, `sbunix-skeleton/docs/3rd-party-install.md`.
- Confidence: High.

2. Spawn/exec ABI compatibility
- Subsystem: process creation, `execve`, wait semantics.
- Problem: pdpmake’s real path uses `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)`, pipes, `/bin/sh`, and `wait4`; SBUnix currently rejects the needed clone shape.
- Inspect: `kernel/syscall_process.c`, `kernel/exec.c`, `kernel/trap.c`.
- Confidence: High.

3. TCC private-dir and sysroot pathing
- Subsystem: exec/runtime layout/toolchain ABI.
- Problem: vendored TCC starts, then searches host-era paths for headers, crt objects, libc, and `libtcc1.a`.
- Inspect: `thirdparty/bin/tcc`, `thirdparty/tcc-sysroot`, `docs/3rd-party-install.md`.
- Confidence: High.

4. `/proc` implementation and exact-row proof
- Subsystem: VFS/procfs.
- Problem: local `procfstest` passes, but the exact external `/proc` row was deferred; current output must be treated as provisional.
- Inspect: `kernel/procfs.c`, `tests/procfstest/procfstest.c`, `kernel/exec.c`, `kernel/proc.c`.
- Confidence: Medium.

5. BusyBox and broader POSIX seam failures
- Subsystem: VFS, TTY, metadata, timers, signals, scheduler, file access.
- Problem: BusyBox only has a minimal applet set, and the broad suites still show failures in `faccessat`, `utimensat`, `fchownat`, `tcgetpgrp`, timers, and scheduler behavior.
- Inspect: `kernel/syscall_io.c`, `kernel/syscall_misc.c`, `kernel/tty.c`, `kernel/syscall_time.c`, `kernel/syscall_signal.c`, `libc/sched.c`, `libc/time.c`.
- Confidence: Medium.

## 3. Third-party binary enablement strategy
- Use the **hybrid** strategy: bring up the vendored binaries first to expose real kernel/runtime bugs, then rebuild or retarget only the binaries that are provably guest-hostile.
- Canonical rebuild instructions come from `sbunix-skeleton/docs/3rd-party-install.md`.
- Early validation may use `/test` for bulky assets, but final functional proof must also work from the normal boot image when that matters for the specific row.
- Submission packaging is deferred. Only local/worktree guest staging is required in this pass.
- Agent A is the single staging/promoter owner for all guest-visible third-party payloads. Workload-specific agents may build and smoke-test artifacts, but only Agent A may promote them into `build/rootfs/bin/{micropython,tcc,pdpmake,busybox}`, the `/bin/make -> pdpmake` alias, `build/rootfs/usr/lib/tcc/{include,lib}`, the BusyBox applet symlink tree, or any mirrored `build/testfs/...` layouts used for bulky diagnostics.
- Agent A maps the host-side TCC stage outputs described in `docs/3rd-party-install.md` into the guest runtime layout. The host staging tree may live under `/home/adebayo/third-party-stage/...`, but the final guest-visible TCC runtime path is `/usr/lib/tcc`, and the promotion rules must make that mapping explicit.
- Agent A also owns runtime environment conventions for imported tools: validate `/bin/sh`, default `PATH`, `HOME`, and `SHELL`, and any updates needed in both `bin/init/init.c` and `rootfs/etc/rc` so pdpmake and BusyBox find commands by normal guest paths instead of accidental host-era assumptions.

## 4. MicroPython integration plan
- Expectations: `execve`, `mmap`, `brk`, `openat`, `readlinkat`, `getcwd`, `getdents64`, stdio, and sane user memory layout.
- Current diagnosis: vendored `micropython` is the least hostile payload and already passes host-side smoke probes.
- Ranked fixes:
1. Recommended minimal fix: stage the vendored binary first for kernel/runtime smoke, then follow `docs/3rd-party-install.md` to prove the documented source-build path by running host `make -C mpy-cross`, `make submodules`, and the static RV64 Unix-port cross-build before calling the workload complete.
2. Broader cleanup: add guest-visible module/data paths only if the exact workload needs them.
3. Invasive fallback: rebuild MicroPython only if the vendored interpreter exposes a guest-specific kernel/runtime defect that cannot be isolated otherwise.
- Validation: vendored-binary startup, `print(42)`, script run from file, `os.getcwd()`, `os.listdir('.')`, read/write under `/mnt`, and a successful documented source rebuild that yields a static RV64 `lp64` executable with no `PT_INTERP`.
- Confidence: Medium-high.

## 5. tcc + pdpmake integration plan
- Treat TCC and pdpmake as related but not identical workstreams.
- TCC problem restatement: the binary starts, but guest-side compile/link is blocked by private-dir/sysroot mismatch.
- TCC ranked fixes:
1. Recommended minimal-correct fix: rebuild or retarget TCC so its guest-visible private dir is **exactly** `/usr/lib/tcc`, with headers under `/usr/lib/tcc/include` and support files under `/usr/lib/tcc/lib`; `tcc -vv` must resolve there, and the final binary must not require host-era `/home/adebayo/...` search paths. If a temporary bootstrap wrapper is absolutely necessary, keep it out of the final command path as `/test/bin/tcc-bootstrap`, never as `/bin/tcc`, and delete it before final compile/link acceptance; final acceptance requires the rebuilt or retargeted binary to discover `/usr/lib/tcc` correctly by default.
2. Acceptable broader cleanup: keep the vendored binary only for startup smoke while using a rebuilt/retargeted binary for compile/link validation.
3. Reject: binary-patch the current TCC to `~/CSE506/...`; it is brittle, semantically wrong, and not a valid guest path strategy.
- pdpmake problem restatement: basic make execution depends on vfork-like clone semantics, shell exec, pipes, wait, and timestamps.
- pdpmake ranked fixes:
1. Recommended minimal-correct fix: build `pdpmake` from source per `docs/3rd-party-install.md`, then add a narrow `CLONE_VM|CLONE_VFORK|SIGCHLD` compatibility path that suspends the parent until child `exec` or `_exit`, wakes the parent on that transition, and keeps all other `CLONE_VM` shapes rejected so SBUnix does not accidentally grow generic shared-mm clone semantics.
2. Acceptable fallback: rebuild pdpmake against a plain `fork` path only if the kernel compatibility path proves too risky.
3. Defer: full shared-mm clone ownership redesign.
- Validation:
1. pdpmake basic first: stage `/bin/pdpmake` plus `/bin/make -> pdpmake`, then confirm `/bin/sh -c true`, `make --version` or equivalent identity proof, PATH-based command lookup, shell-backed recipe execution, dependency rerun, and timestamp-sensitive rule behavior before any TCC-backed build.
2. TCC second: `tcc -v`, `tcc -vv`, `tcc hello.c -o hello`, `./hello`.
3. pdpmake build last: pdpmake-driven TCC build only after the TCC path is clean.
- Confidence: Medium.

## 6. busybox integration plan
- Expectations: robust `execve`, `openat`, `newfstatat`, `getdents64`, `ioctl(TIOCGWINSZ)`, applet dispatch, and correct file/text utility behavior.
- Current diagnosis: the vendored BusyBox is fine for initial `busybox <applet>` smoke but too small for full file/text rows.
- Ranked fixes:
1. Recommended minimal-correct fix: smoke-test the vendored binary, then rebuild BusyBox with the required fs/text applets using the canonical instructions in `docs/3rd-party-install.md`, and preserve the applet symlink tree.
2. Acceptable cleanup: add a minimal compatibility handler for the exact `prctl` request BusyBox needs if startup proves blocked by that single syscall.
3. Broader fallback: expand applet coverage further only if the exact rows require more than file/text utilities.
- Validation: `busybox --list`, direct invocation, symlink invocation like `/bin/ls`, PATH-based invocation from the guest shell, fs applets, and text applets.
- Confidence: Medium.

## 7. Root-cause-analysis and ranked-diagnosis workflow
- Restate each failure in subsystem terms and say whether it is loader, ABI, syscall-semantics, filesystem, runtime-layout, or maintainability debt.
- Capture evidence before patching: serial log, targeted repro, binary metadata, and `qemu-gdb` when the failure is ambiguous.
- Rank causes as primary, secondary, or masked.
- Rank fixes as minimal-correct, broader cleanup, and invasive redesign.
- Choose one fix and state whether the next move is patch-forward, instrument-first, isolate-with-test-first, or refactor-first.
- Record confidence as High, Medium, or Low in `test-findings-20.md`.

## 8. Evaluation-feedback unresolved-failure remediation plan
- Must-fix-now rows: exact `exec`, MicroPython, TCC basic, TCC compile/run, pdpmake basic/build, BusyBox basic/fs/text, and the `/proc` row.
- `/proc` is explicit here: treat `kernel/procfs.c` as partially implemented until the exact row is exercised and any parser/format mismatches are fixed.
- High-value blocking seams to close first: `clone`/spawn ABI, `faccessat`, `utimensat`, `fchownat`, `tcgetpgrp`, timer breadth, and signal queue/wait behavior.
- Explicitly defer wide non-blockers unless a target binary trips them: broad complex/math, wchar, dlfcn, locale, and unrelated libc surface area.

## 9. Recommended file split/refactor plan
- Priority 1, safe early split: extract third-party staging into a focused make fragment or staging helper instead of leaving it tangled in the main `Makefile`.
- Justification: highest correctness-risk reduction for guest payload promotion, easiest review surface, highest parallelization benefit, and best immediate testability gain.
- Priority 2, safe early split: split `kernel/syscall_process.c` only if the vfork/exec work starts mixing too much unrelated logic.
- Justification: high correctness-risk reduction around clone/exec semantics, medium review benefit, medium parallelization benefit, and high maintainability gain if the file starts to sprawl further.
- Priority 3, safe early split: extract common exec/path/shebang helpers so tarfs and VFS exec paths stop diverging.
- Justification: medium correctness-risk reduction, high review clarity, medium testability gain, and medium maintainability improvement.
- Deferred cleanup 1: large `ext2.c` cleanup after correctness-critical work lands.
- Justification: medium-long-term maintainability win, but lower short-term correctness payoff for the target workloads.
- Deferred cleanup 2: large `syscall_ipc.c` cleanup after correctness-critical work lands.
- Justification: medium maintainability and review benefit, but not a current third-party blocker.
- Deferred cleanup 3: large `tty.c` cleanup beyond the exact PTY/job-control fixes needed now.
- Justification: medium testability benefit, but defer the broad split until the immediate PTY and job-control failures are fixed.

## 10. Parallel multi-agent assignment plan
- Wave 0, serial: inventory assets, set up worktrees, sync submodules, create a tmux session with one pane per active agent/build/test job, build the TODO list, and verify the host `riscv64-linux-musl` toolchain from `docs/3rd-party-install.md`; if it is missing or misconfigured, build/install it first and run the documented smoke compile plus `file`/`readelf` checks before any workload wave begins.
- Wave 0, serial: complete the `plan-03.md` audit table and the artifact-backed evidence matrix from `evaluation-summary.md`, `test-expectations.md`, `test-findings-19.md`, and `test-findings-19-artifacts/` before ranking the implementation order.
- Wave 1, parallel:
- Agent A: third-party staging/runtime layout.
- Agent B: clone/exec/wait ABI work.
- Agent C: `/proc` and PTY/job-control seam triage only.
- Agent D: metadata, timer, signal, and scheduler seam triage only.
- Wave 2, parallel after Wave 1:
- Agent E: MicroPython bring-up and documented source-build proof.
- Agent B with coordinator support: pdpmake basic validation as soon as the clone fix and guest shell environment land.
- Agent F: TCC retarget/rebuild and compile path.
- Agent G: BusyBox config and applet-tree build/validation.
- Wave 3, serial: pdpmake-driven TCC build, final regression, and reporting.
- During execution, keep active agents/builds/tests in separate tmux panes and monitor straggling build or test processes before starting any dependent wave.

## 11. Per-agent responsibilities, context, and required skills
- Coordinator:
- Context: `CLAUDE.md`, `plan-03.md`, `evaluation-summary.md`, `test-expectations.md`, `test-findings-19.md`, `test-findings-19-artifacts`, `docs/3rd-party-install.md`.
- Skills: `$brainstorming`, `$os-engineer`, `$writing-plans`.
- Coordinator owns: host `riscv64-linux-musl` toolchain bootstrap or verification before any third-party build starts, plus the final merge sequence back into `os-auxillary`.
- Agent A owns: `Makefile`, `thirdparty/Makefile`, rootfs/testfs staging, symlink preservation, runtime environment checks in `bin/init/init.c`, `rootfs/etc/rc`, `/bin/sh`, and default `PATH`/`HOME`/`SHELL`, plus all promotion of rebuilt third-party artifacts into `build/rootfs/...`, the `/bin/make -> pdpmake` alias, or mirrored `build/testfs/...` guest-visible paths.
- Agent B owns: `kernel/syscall_process.c`, `kernel/exec.c`, related wait/clone helpers, the `pdpmake` source build, and pdpmake basic validation once the vfork-like path lands.
- Agent C owns: `kernel/procfs.c`, PTY/job-control pieces in `kernel/tty.c` and related procfs-facing path checks.
- Agent D owns: metadata, timer, signal, scheduler, and related libc shim fixes in `kernel/syscall_io.c`, `kernel/syscall_misc.c`, `kernel/syscall_time.c`, `kernel/syscall_signal.c`, `libc/time.c`, and `libc/sched.c`.
- Binary-specific agents own only their workload build/bring-up/validation. They do not own shared image staging rules or final payload promotion.

## 12. Worktree/build/test workflow
- Keep the coordinator tree on `os-auxillary`. Use dedicated worktrees for builds/tests.
- Every build or test worktree starts from a short-lived branch forked from `os-auxillary`.
- Simultaneous multiple builds are allowed when they run in different worktrees. Do not stack independent build trees into one checkout and pretend the artifacts are isolated.
- Toolchain bootstrap or repair for `riscv64-linux-musl` happens first in a clean host shell with the cross variables unset, exactly as `docs/3rd-party-install.md` requires. Only after that prerequisite succeeds should a worktree shell run `cd sbunix-skeleton` and `source tools/sourceme.sh` for repo-local build, boot, or validation commands.
- In every worktree: `git submodule update --init --recursive` before building.
- Each worktree stages only into its own local `build/rootfs` and `build/testfs` trees. Agent A serializes final payload promotion and merge-back so two worktrees never try to publish guest-visible artifacts into the same checkout at the same time.
- After each large third-party build is staged and validated, delete or archive the no-longer-needed source/build tree in that worktree so parallel builds do not exhaust disk on avoidable leftovers.
- Rebuild after every relevant source change. No stale-build comparisons.
- Stale-build rules:
- `rootfs` or final image staging change: rebuild tarfs.
- `/test` payload change: rebuild `build/test-disk.img`.
- `kernel` or `libc` change: full rebuild before validation.
- Host sizing for this machine:
- one heavy build: `make -j4`
- two heavy builds in parallel: `make -j2` each
- more than two heavy builds, or build plus active QEMU/GDB: `make -j1`
- After targeted validation passes in a worktree, merge that branch back into `os-auxillary` with `MERGE:`-prefixed commit messages.
- Submission packaging is deferred for this plan.

## 13. Targeted validation strategy
- Fixed order:
1. focused failing repro
2. issue-specific test
3. binary-specific execution check
4. focused repro for any kernel bug uncovered by that binary
5. single full-suite pass later
- Use GDB on `sys_clone_handler`, `sys_execve_handler`, `exec_vfs`, `sys_faccessat_handler`, `sys_ioctl_handler`, `tty_ioctl`, page-fault handlers, `copyin`, and `copyout` when diagnosis is ambiguous.
- Before guest smoke for any rebuilt workload binary, run the documented `file` and `readelf` checks to confirm static RV64, soft-float `lp64`, and no `PT_INTERP` for MicroPython, TCC, pdpmake, and BusyBox.
- If new tests are added during remediation, they must be rigorous, unmocked, and non-trivial; reject shallow tests that only prove the happy path or restate the implementation.
- Required binary checks:
- MicroPython: startup, `-c`, file script, `import os`, file I/O.
- TCC: startup, `-vv`, compile, execute compiled program.
- pdpmake: `/bin/sh -c true`, PATH-based command lookup, shell-backed recipe, and dependency rebuild before any TCC-backed build.
- BusyBox: direct applet, symlink applet, PATH-based invocation, fs/text utilities.
- `/proc`: exact row plus parser-sensitive reads of `/proc/self/*`, `/proc/<pid>/*`, and system-wide proc files.

## 14. Full regression strategy
- Run the full suite once, after targeted validations are green enough to justify it.
- Use a fresh merged worktree, rebuild there from a clean state, and run the repo’s host runner with `bash tools/run_tests.sh` or an equivalent `make test` path that performs the two-boot persistence flow. The final pass must still capture the `os-test` and `open-posix` results from that run rather than substituting a narrower ad hoc sequence.
- Save serial logs, extract fail lists, and classify each failure as fixed, unchanged, regressed, or newly exposed.
- If the full pass finds real failures, record them instead of rerunning out of panic.

## 15. Post-change Codex review workflow
- After each significant merge wave, run `codex:adversarial-review`.
- Then run `codex:rescue`.
- For any change in an existing file, add a short why-comment where the logic changed, even when the edit feels locally obvious to the implementer.
- Every commit must follow the repo’s prefix rules and capture the rationale cleanly without banned test names or other repo-forbidden grading vocabulary.

## 16. Final reporting and notification workflow
- After the single full regression run, write `/home/adebayo/test-findings/test-findings-20.md` as a clean structured report.
- Include: implemented work, fixed issues, new passes, remaining failures, newly exposed issues, suspected root causes, confidence per remaining issue, commands/tests run, MicroPython results, TCC/pdpmake results, BusyBox results, `/proc` results, and remaining blockers.
- Send completion notification with `ntfy-notify` to `https://ntfy.sh/ab-mac`.
- Because this is still a review-only pass, `plan-04.md` remains the reviewed artifact and must not trigger implementation in this session.
