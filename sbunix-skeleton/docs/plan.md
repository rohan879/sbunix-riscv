# RV64IMAC SBUnix CR07 Remediation Plan

## 1. Global Diagnosis Summary

Six parallel `plan-review-cdx` rounds are complete. The final round found two execution blockers in validation logging; both are corrected here. Completion notification was sent to `https://ntfy.sh/ab-mac`.

Baseline is current `/home/adebayo/CSE506` on `os-debug-01` at observed HEAD `148594d`, not stale commit `de4e1ae`. Feedback from `de4e1ae` remains required context, but no remediation branch starts until current `os-debug-01` has reproducible baseline artifacts, raw QEMU logs, a stub/no-op audit, and a 49-row matrix in `/home/adebayo/test-findings/test-findings-19.md`.

Treat the kernel as untrusted. The main risks are user-controlled trap return, silent PTE mutation, repair paths hiding `PTE_X` corruption, broken process/VMA ownership, copyout-after-mutation syscalls, and VFS/ext2 behavior that works only accidentally.

## 2. Ranked Issue Clusters By Priority

1. Current baseline and matrix: non-fatal host baseline capture, raw boot logs, exact 49-row status matrix, and stub/no-op audit.
2. Immediate safety fixes: `CR07-001`, `CR07-005`, `CR07-006`, `CR07-013`.
3. Urgent page-table / `PTE_X`: fail-fast repair mode, `pte_update`, typed raw-PTE-write audit, page-frame ownership, zero repair events.
4. Trap/signal/time correctness: `CR07-004`, `CR07-010`, `CR07-011`, `CR07-014`, `CR07-015`, RTC wall-clock.
5. VM/VMA/process lifetime: `CR07-002`, `CR07-003`, `CR07-007`, `CR07-008`, `CR07-017`, lazy allocation.
6. Path/VFS/procfs/ext2: `CR07-009`, `CR07-012`, `CR07-016`, symlink, procfs, persistence.
7. Architecture debt: only correctness-driven refactors before broad cleanup.

## 3. Autograder Unresolved-Failure Remediation Plan

The report matrix must contain exactly these 49 rows, in order, with no collapsing or renaming:

`01_syscall`, `02_memory`, `03_string`, `04_fdio`, `05_fork`, `06_exec`, `07_python`, `08_dirops`, `09_environ`, `10_pipe`, `11_signals`, `12_sighand`, `13_procfs`, `14_writable`, `15_make`, `15_make_build`, `16_tcc`, `16_tcc_compile`, `17_cow`, `18_lazy`, `19_pgrp`, `20_symlink`, `21_wallclock`, `22_busybox`, `22_busybox_fs`, `22_busybox_txt`, `23_persist_w`, `23_persist_v`, `sh_echo`, `sh_exit_code`, `sh_builtin_cd`, `sh_path_lookup`, `sh_pipe`, `sh_pipe_chain`, `sh_pipe_grep`, `sh_redir_out`, `sh_redir_append`, `sh_redir_in`, `sh_background`, `sh_script`, `sh_script_args`, `sh_cleanup`, `sh_ctrl_u`, `sh_ctrl_c`, `sh_ctrl_z`, `sh_jobs`, `sh_fg`, `sh_ctrl_backslash`, `sh_ctrl_d`.

Required columns: `row`, `prior_state`, `current_baseline`, `likely_subsystem`, `workstream`, `targeted_validation`, `final_status`, `confidence`, `notes`.

Allowed unknowns: `N/A`, `Blocked:<reason>`, `Not run:<reason>`. Final statuses must be `PASS`, `Blocked:<infrastructure/user-approved reason>`, or `Deferred:<owner,rationale,next validation>`. A row may not remain merely "failed but classified."

Baseline stub/no-op audit, from `/home/adebayo/CSE506/sbunix-skeleton`:

```bash
rg -n "stub|TODO|ENOSYS|return 0;|not implemented|no-op|unsupported" kernel libc bin
```

Every hit touching a 49-row area must be classified before branch creation as `implemented-correct`, `explicitly-unsupported-with-errno`, or `blocking`. Repeat the audit before final gates.

## 4. Urgent Page-Table / PTE_X Corruption Investigation Plan

Inspect `kernel/vm.c`, `kernel/include/riscv.h`, `kernel/include/vm.h`, `kernel/trap.c`, `kernel/kernel.c`, `kernel/exec.c`, `kernel/swap.c`, `kernel/anon_vma.c`, `kernel/syscall_memory.c`, `kernel/syscall_process.c`, `kernel/kalloc.c`, `kernel/kmalloc.c`, and `kernel/virtio_blk.c`.

Mandatory changes:

- Add `PTE_DEBUG_FAILFAST=1` build support and a boot log proving it is active.
- Add `pte_update()` for every non-initialization PTE write. It records VA, old PTE, new PTE, reason, VA class, and `sfence.vma` requirement.
- Enforce Sv39: non-leaf PTEs have no RWX; leaf PTEs are not page-table pages; kernel/MMIO pages never have `PTE_U` or unexpected `PTE_X`.
- Do not silently change user W^X behavior. Preserve current ELF/`mprotect` compatibility unless a separate audited policy lands.
- Add debug page-frame tracking: `PAGE_FREE`, `PAGE_PAGETABLE`, `PAGE_USER`, `PAGE_SLAB`, `PAGE_DMA`, `PAGE_KERNEL`, `PAGE_UNKNOWN`.

Typed raw-PTE-write audit from `<worktree>/sbunix-skeleton`:

```bash
rg -n '(^|[^A-Za-z0-9_])\*[A-Za-z0-9_]*(pte|PTE)[A-Za-z0-9_]*\s*=|\b[A-Za-z0-9_]*(pagetable|pt|L[0-9]|l[0-9]|kernel_pagetable|user_L1)[A-Za-z0-9_]*\s*\[[^]]+\]\s*=' kernel
```

Every hit is either allowlisted construction/destruction zeroing or converted to `pte_update(..., reason, sfence_requirement)`.

Fail-fast acceptance requires: fail-fast mode enabled, no `kernel PTE repair`, no `PTE repair`, no `panic`, no unexpected `WARNING`, and no page-table page that is free, slab-owned, DMA-targeted, or mapped as a user leaf.

## 5. Detailed Remediation Plan For CR07-001 Through CR07-017

- `CR07-001`: Fix `rt_sigreturn`. Never restore user-supplied `sstatus`. Derive status from kernel policy only: SPP clear, SPIE per trap-return policy, hardcoded safe bits only. Restore only user ABI GPRs, user `sp`, user `sepc`, and signal mask.
- `CR07-002`: Block eviction of `page_ref_get(pa) > 1`, COW, and shared-anon pages until rmap is authoritative; then converge page ownership metadata.
- `CR07-003`: Implement shared `mm` ownership: `proc->mm->pagetable`, `vma_list`, `mmap_base`, refcount; update exec, exit/free, scheduler `satp`, trap lookup, copyin/out ownership, swap/rmap.
- `CR07-004`: Validate signal handler VA and permissions before trapframe or signal-state mutation.
- `CR07-005`: Fix `readlinkat` stack overflow by capping to `sizeof(kbuf)` or using bounded heap allocation.
- `CR07-006`: Make `wait4` copy status/rusage before irreversible reap, or implement rollback-safe transaction.
- `CR07-007`: Refuse anon VMA merge unless metadata merge is lossless, or implement metadata merge.
- `CR07-008`: Add one VFS close/drop helper and use it for file-backed VMA release.
- `CR07-009`: Implement ext2 truncate shrink release, grow zero-fill, and inode persistence.
- `CR07-010`: Implement timed wait for `sigtimedwait`, with explicit wake reason and timeout result.
- `CR07-011`: Make nanosleep wake on signal, return interruption, and copy remaining time.
- `CR07-012`: Add one dirfd-aware resolver for every implemented `*at`: openat, newfstatat, faccessat, unlinkat, linkat, renameat2, mkdirat, mknodat, readlinkat, symlinkat, fchmodat, utimensat, fchownat.
- `CR07-013`: Validate `sigsetsize`; unsupported sizes return `EINVAL`.
- `CR07-014`: Centralize signal permission checks for kill/tgkill/rt_sigqueueinfo.
- `CR07-015`: Build valid `siginfo_t` and `ucontext_t` for `SA_SIGINFO`.
- `CR07-016`: Reject directory hardlinks at syscall/VFS level; ext2 also rejects defensively.
- `CR07-017`: Preallocate/link all VMA segments before `mprotect` mutation or rollback exactly.

Each issue needs pre-fix proof, ranked options in branch notes, selected fix, post-fix log under `/home/adebayo/test-findings/test-findings-19-artifacts/<branch>/<issue>.log`, and confidence.

## 6. Architectural Debt Remediation Plan

Mandatory correctness refactors:

- `pte_update` and typed raw-write audit.
- Central trap-return sanitizer.
- State/copyout transaction pattern for syscalls.
- VFS close/drop helper.
- Shared `mm` owner for `CLONE_VM`.

Broad file splits are deferred until behavioral fixes in those files land and must be mechanical with compile-only validation.

## 7. Single-Hart-Masked / Lower-Priority Risk Triage Plan

Fix now despite single-hart: address-space lifetime, refcount/close ordering, trap-return exposure, MMIO permission/order bugs, implicated virtio barriers, and PTE mutation without `sfence.vma`.

Defer only true SMP scheduler/lock cleanup with written backlog. Single-hart execution is not an excuse for stale ownership or bad privilege invariants.

## 8. Follow-Up Validation Plan

Common branch prelude:

```bash
mkdir -p /home/adebayo/test-findings/test-findings-19-artifacts/<branch>
cd <worktree>/sbunix-skeleton
source /home/adebayo/CSE506/sbunix-skeleton/tools/sourceme.sh
make clean
BUILD_TESTS=1 make -j<N> build/kernel.elf build/disk.img build/test-disk.img
```

Use this validation helper in each branch shell, replacing `<branch>`:

```bash
branch=<branch>
worktree=/home/adebayo/CSE506-<branch>
: "${branch:?set branch}"
: "${worktree:?set worktree}"
artifact_dir=/home/adebayo/test-findings/test-findings-19-artifacts/${branch}
mkdir -p "$artifact_dir"

run_guest() {
  id="$1"; guest_cmd="$2"; timeout_s="${3:-300}"
  session="${branch}-${id}-$$"
  log="${artifact_dir}/${id}.log"
  raw_log="${artifact_dir}/${id}.serial.log"
  : > "$raw_log"
  tmux new-session -d -s "$session" "cd ${worktree}/sbunix-skeleton && source /home/adebayo/CSE506/sbunix-skeleton/tools/sourceme.sh && BUILD_TESTS=1 make qemu"
  tmux set-option -t "$session" history-limit 200000 >/dev/null
  tmux pipe-pane -o -t "$session" "cat >> '$raw_log'"
  deadline=$((SECONDS + timeout_s))
  until tmux capture-pane -p -t "$session" | rg -q 'Starting tests in: 3|sbush\$'; do [ "$SECONDS" -lt "$deadline" ] || break; sleep 1; done
  tmux send-keys -t "$session" C-c
  until tmux capture-pane -p -t "$session" | rg -q 'sbush\$'; do [ "$SECONDS" -lt "$deadline" ] || { tmux kill-session -t "$session" || true; return 124; }; sleep 1; done
  tmux send-keys -t "$session" "echo __START:${id}; ${guest_cmd}; echo __STATUS:\$?; echo __END:${id}" Enter
  until tmux capture-pane -p -t "$session" | rg -q "__END:${id}"; do [ "$SECONDS" -lt "$deadline" ] || { tmux kill-session -t "$session" || true; return 124; }; sleep 1; done
  tmux capture-pane -pS - -t "$session" > "$log"
  tmux send-keys -t "$session" poweroff Enter || true
  sleep 2; tmux kill-session -t "$session" || true
  cat "$raw_log" >> "$log"
  rg -q "__STATUS:0" "$raw_log"
  ! rg -q '\[FAIL\]|panic|kernel PTE repair|PTE repair|WARNING' "$raw_log"
}
```

Pass criteria: `__STATUS:0`, expected success marker, no `[FAIL]`, no panic/PTE repair/warning unless allowlisted, no timeout/contamination. `run-open-posix-test <t>` must print exactly `PASS: <t>`; reject `UNRESOLVED`, `UNSUPPORTED`, `UNTESTED`, `SKIP`, `FAIL`, and `Test not found`.

## 9. Recommended File Split / Refactor Plan

- Early: `pte_update` helper only.
- Deferred: split `vm.c`, split signal ABI from validation, consolidate dirfd/path resolver, split ext2 truncate/allocation/link policy.
- No split may run parallel with behavioral edits to the same file.

## 10. Parallel Multi-Agent Assignment Plan

Baseline is artifact-only; no `rem-baseline` branch. Create dependent worktrees only after prerequisites merge.

| Branch | Creation point | Write scope | Merge gate |
|---|---|---|---|
| `rem-safety-abi` | After baseline matrix | signal/io/process safety files, minimal headers, focused tests; `trap.c` only for CR07-001/004 | `/test/bin/trapretstress`, new sigreturn/readlink/wait tests, targeted signal/readlink/wait os-tests |
| `rem-pte` | After safety merge | VM/PTE/kalloc/kmalloc/virtio and PTE-touching paths | fail-fast mode, typed PTE audit, GDB artifact, 200x `/test/bin/ptextest`, stress tests, zero repairs |
| `rem-signal-time` | After PTE if `trap.c` overlaps, otherwise after safety | signal/time, `trap.c` timer wake path, `proc.h` wake reason, RTC/timekeeping | sigtimedwait/nanosleep Open POSIX targets, `/test/bin/clocktest`, `/test/bin/rtctest`, realtime > 1B |
| `rem-vm-lifetime` | After PTE | shared mm, exec/trap/swap/vm/mm/anon/process/vfs | exec/lazy/copyin/out/COW/swap/mmap/mprotect tests, clone VM, rollback tests |
| `rem-vfs-path` | After safety and `syscall_io.c` conflicts | pathres, io/misc syscalls, VFS, procfs, link policy | symlink/fs/procfs, dirfd matrix, dir hardlink reject |
| `rem-ext2` | After vfs-path if conflicts; otherwise after baseline | ext2 only | fstest, truncate matrix, persistence two-boot |
| `rem-validation` | After fix branches | tests/reports/artifacts only | exact 13-section report complete |

Each branch may add narrowly scoped non-trivial tests under `tests/<name>/<name>.c`; they must fail pre-fix and run as `/test/bin/<name>`.

## 11. Per-Agent Responsibilities, Context, And Required Skills

Every agent reads:

- `/home/adebayo/CSE506/CLAUDE.md`
- `/home/adebayo/autograder-feedback/evaluation-summary.md`
- `/home/adebayo/autograder-feedback/test-expectations.md`
- `/home/adebayo/test-findings/code-review-07.md`
- `/home/adebayo/test-findings/code-review-07-artifacts`
- Its worktree source

Raw evaluation JSON may be read only for external planning/report artifacts. Use `$os-engineer` judgment for RISC-V trap, VM, ABI, MMIO, and syscall decisions.

## 12. Worktree / Build / Test / Merge / Cleanup Workflow

Baseline preflight:

```bash
set -euo pipefail
mkdir -p /home/adebayo/test-findings/test-findings-19-artifacts/baseline
cd /home/adebayo/CSE506
git switch os-debug-01
staged=$(git diff --cached --name-only)
dirty_disallowed=$(git status --porcelain=v1 --untracked-files=all | awk '$2!="AGENTS.md" && $2!="CLAUDE.md" {print}')
if [ -n "$staged" ] || [ -n "$dirty_disallowed" ]; then
  printf '%s\n' "$dirty_disallowed" > /home/adebayo/test-findings/test-findings-19-artifacts/baseline/dirty-blockers.txt
  echo "Blocked:dirty-state"
  exit 2
fi
```

Build baseline and save clean images:

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
git submodule update --init --recursive
source /home/adebayo/CSE506/sbunix-skeleton/tools/sourceme.sh
make clean
BUILD_TESTS=1 make -j4 build/kernel.elf build/disk.img build/test-disk.img
cp build/disk.img /home/adebayo/test-findings/test-findings-19-artifacts/baseline/disk.clean.img
cp build/test-disk.img /home/adebayo/test-findings/test-findings-19-artifacts/baseline/test-disk.clean.img
sha256sum /home/adebayo/test-findings/test-findings-19-artifacts/baseline/*.img > /home/adebayo/test-findings/test-findings-19-artifacts/baseline/image-sha256.txt
```

Raw baseline capture: restore clean images, capture boot 1 to `baseline/qemu-boot1.raw.log`; without restoring, boot again and capture boot 2 to `baseline/qemu-boot2.raw.log`; then restore clean images again before host summary. Each boot uses tmux, 900s timeout, pane pipe logging, teardown, and QEMU leak check.

Host baseline summary:

```bash
set +e
TIMEOUT=900 bash tools/run_tests.sh 2>&1 | tee /home/adebayo/test-findings/test-findings-19-artifacts/baseline/host-run-tests.log
status=${PIPESTATUS[0]}
set -e
echo "$status" > /home/adebayo/test-findings/test-findings-19-artifacts/baseline/host-run-tests.status
if rg -q '^FATAL:|Total:[[:space:]]+0' /home/adebayo/test-findings/test-findings-19-artifacts/baseline/host-run-tests.log; then
  echo "Blocked:infrastructure" > /home/adebayo/test-findings/test-findings-19-artifacts/baseline/baseline-blocked.txt
  exit 2
fi
rg -q 'SBUnix Test Results' /home/adebayo/test-findings/test-findings-19-artifacts/baseline/host-run-tests.log
```

Worktree creation preflight:

```bash
cd /home/adebayo/CSE506
git worktree list --porcelain | rg -q '^worktree /home/adebayo/CSE506-<branch>$' && exit 2
git show-ref --verify --quiet refs/heads/<branch> && exit 2
test ! -e /home/adebayo/CSE506-<branch> || exit 2
git worktree add -b <branch> /home/adebayo/CSE506-<branch> os-debug-01
cd /home/adebayo/CSE506-<branch>/sbunix-skeleton
git submodule update --init --recursive
source /home/adebayo/CSE506/sbunix-skeleton/tools/sourceme.sh
```

Build parallelism: 4 CPUs; `make -j4` for one job, `make -j2` for two concurrent jobs; never more than two QEMU-heavy jobs.

Merge gate:

- No staged files.
- No dirty tracked files except allowed `AGENTS.md`; untracked `CLAUDE.md` allowed but never staged.
- Record hashes/diffs for `AGENTS.md` and `CLAUDE.md` before merge; verify unchanged after.
- Merge verified branches into `os-debug-01` with `MERGE:` prefix.
- If post-merge tests fail, fix forward on `os-debug-01` or revert the merge, create `<branch>-redo` from current `os-debug-01`, cherry-pick verified commits, fix/revalidate, then merge redo. Do not re-merge an already reverted branch unchanged.
- Cleanup only after successful final gates. Remove clean worktrees only after QEMU leak check passes. Delete merged branches with `git branch -d`.

## 13. Targeted Validation Strategy

Run Section 10 branch commands through `run_guest`.

PTE loop:

```bash
for i in $(seq 1 200); do run_guest "pte-$i" "/test/bin/ptextest" 1800; done
run_guest "trapretstress" "/test/bin/trapretstress"
run_guest "membugstest" "/test/bin/membugstest"
run_guest "virtiostress" "/test/bin/virtiostress"
```

Two clean boots means two complete `BUILD_TESTS=1 make qemu` boots captured through tmux, each reaching `sbush$`, with no panic, no PTE repair, and no timeout.

## 14. Full Regression Strategy

Full third-party suites run once per final candidate after targeted gates and merges. If a full run exposes a bug and code/test/harness changes are made, that candidate is invalid; rerun once for the new final candidate and log every attempt. Do not duplicate a successful final run or run concurrent full suites.

- `run-os-test`: `run_guest "final-os-test" "run-os-test" 5400`.
- `run-open-posix-test`: `run_guest "final-open-posix" "run-open-posix-test" 3600`.
- Host two-boot summary: non-fatal status capture like baseline, stored under `final/host-run-tests.*`.

Final acceptance: every 49-row entry is `PASS`, `Blocked:<infrastructure/user-approved reason>`, or `Deferred:<owner,rationale,next validation>`; no changed-cluster regression; zero PTE repair events.

## 15. Post-Change Codex Review Workflow

From each branch worktree:

```bash
mkdir -p /home/adebayo/test-findings/test-findings-19-artifacts/reviews

# Before committing, review dirty branch changes; after committing, omit --uncommitted to review branch diff.
codex review --base os-debug-01 --uncommitted - <<'PROMPT' > /home/adebayo/test-findings/test-findings-19-artifacts/reviews/<branch>-adversarial.md
Bad-mood senior RV64IMAC SBUnix code review. Prioritize correctness bugs, trap/VM/PTE/ABI regressions, missing tests, and architecture drift. Findings only, severity ordered.
PROMPT

codex exec --sandbox danger-full-access - <<'PROMPT' > /home/adebayo/test-findings/test-findings-19-artifacts/reviews/<branch>-rescue.md
Apply only fixes for accepted Critical/High findings from the adversarial review. Do not touch unrelated files. Rebuild and rerun targeted branch validation.
PROMPT
```

This satisfies `codex:adversarial-review` followed by `codex:rescue`. Merge gate: no unresolved Critical/High findings; Medium findings fixed or deferred with OS-kernel justification and report entry.

## 16. Final Reporting And Notification Workflow

`/home/adebayo/test-findings/test-findings-19.md` must use exactly these sections, in order:

1. Run metadata
2. Baseline summary
3. 49-row evaluation matrix
4. CR07 status table
5. PTE/PTE_X investigation and zero-repair evidence
6. Commands/log index
7. Branch/worktree/merge SHA table
8. Fixed items and tests now passing
9. Remaining failures and suspected root causes
10. Newly discovered issues and architectural risks
11. Follow-up validation results
12. Cleanup result
13. Notification result

Empty sections are marked `None` or `Blocked:<reason>`. Do not blindly append.

Notification command:

```bash
mkdir -p /home/adebayo/test-findings/test-findings-19-artifacts/final
notif_log=/home/adebayo/test-findings/test-findings-19-artifacts/final/notification.log
if /home/adebayo/.agents/skills/ntfy-notify/scripts/ntfy_send.sh --topic ab-mac --title "os-debug-01" "<outcome>: /home/adebayo/test-findings/test-findings-19.md" >"$notif_log" 2>&1; then
  echo "success" > /home/adebayo/test-findings/test-findings-19-artifacts/final/notification.status
else
  echo "failure" > /home/adebayo/test-findings/test-findings-19-artifacts/final/notification.status
fi
```

Assumptions and defaults: integration target is `os-debug-01`; current dirty `AGENTS.md` and untracked `CLAUDE.md` are instruction-file state and must not be staged; third-party submodules are initialized recursively in each worktree; full suites are run once per final candidate, not repeatedly for unchanged candidates.
