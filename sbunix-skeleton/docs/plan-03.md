# Remaining CR07 Finalization Implementation Plan

> **For agentic workers:** Use parallel multi-agent workers where tasks are independent. Keep `AGENTS.md` and `CLAUDE.md` out of staging. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the work left after `rem-plan-02` by validating unavailable workloads, running final gates, integrating the branch, and updating the report.

**Architecture:** The source remediation items from `plan-02.md` have landed on `rem-plan-02`. Remaining work is validation, app/workload compatibility capture, final report cleanup, and integration back to `os-debug-01`. Any new source change found during validation must be narrow, tested, committed with the required prefix, and reflected in the report.

**Tech Stack:** SBUnix RV64IMAC, QEMU, tmux, shell validation helpers, `BUILD_TESTS=1`, local report artifacts under `/home/adebayo/test-findings/test-findings-19-artifacts`.

---

## Execution Ownership Rules

- Parallel workers may create artifacts and per-task handoff notes only.
- Only the final integration/report pass may edit `/home/adebayo/test-findings/test-findings-19.md`.
- If a worker discovers evidence that changes a row disposition, it must write the evidence path and proposed disposition to a task-local `handoff.md` under that task's artifact directory.
- The final integration/report pass serializes all report edits after parallel workers finish, then verifies the 49-row count and final disposition grammar.
- No worker may stage or commit `AGENTS.md`, `CLAUDE.md`, or plan files unless the user explicitly asks to track a plan file.

## Stop Notification Rule

If task execution stops for any reason, send an ntfy notification before leaving the session. This applies to normal completion, user-requested pause, blocker, validation failure, tool failure, timeout, disk-space issue, or any other stop condition.

Use this command, changing the message to describe the actual stop reason:

```bash
/home/adebayo/.agents/skills/ntfy-notify/scripts/ntfy_send.sh \
  --topic ab-mac \
  --title "plan-03 stopped" \
  --priority 3 \
  "plan-03 execution stopped: <reason>; next step: <next-step>"
```

Expected:

- The notification command exits with status `0`.
- If notification fails, record the failure in the final response and in `/home/adebayo/test-findings/test-findings-19.md` if the report is being updated.

## Current State

- `rem-plan-02` contains the remaining source fixes from `plan-02.md`.
- Local cleanup already removed obsolete sibling worktrees and redundant local branches. If `os-debug-01` is needed for the merge target and no local branch exists, recreate it from `origin/os-debug-01` before merging.
- Implemented commits:
  - `97f089d BF: Blocked unsafe VM sharing and eviction paths`
  - `fc8a538 BF: Fixed ext2 truncate persistence and directory hardlink checks`
  - `22cf54a BF: Fixed dirfd path resolution and directory hardlink policy`
- The source-fix items formerly listed as remaining are now implemented:
  - `CR07-002`, `CR07-003`, `CR07-007`, `CR07-008`, `CR07-017`
  - `CR07-009`
  - `CR07-012`, `CR07-016`
- The findings report exists at `/home/adebayo/test-findings/test-findings-19.md`.
- The evaluation matrix still has exactly 49 rows.
- Local validation evidence already captured:
  - `BUILD_TESTS=1 make build/test-disk.img` completed.
  - Two direct QEMU boots completed.
  - Parsed result: `1368` pass lines and `0` fail lines.
  - No kernel panic, PTE repair output, or anchored warning lines were found.
  - Stub/no-op audit was rerun.

## Remaining Work Summary

What remains is not another broad source-fix phase. The left-over work is:

- Exact interactive shell and job-control probes where broad local tests passed but exact probes were not isolated.
- The deferred PTE endurance loop.
- Final full-suite candidate validation.
- Required exact branch review workflow from `plan.md`.
- Merge/integration cleanup.
- Report and notification finalization.
- Explicit decision on two residual engineering risks.
- External/third-party workload validation is now a low-priority backlog track. MicroPython, pdpmake, TCC, BusyBox, and any similar externally acquired binary must not block shell/job-control probes, PTE endurance, final regression, review, residual-risk decisions, merge, cleanup, report, or notification work.
- `musl-cross-make` may still be installing outside this plan. Do not wait for it, interrupt it, or make current plan progress depend on it. Record MicroPython as deferred until the toolchain install has completed and the user explicitly reprioritizes external workload validation.

## Task 1: Deferred External Workload Tracking

**Files:**
- Read: `/home/adebayo/test-findings/test-findings-19.md`
- Create handoff notes under: `/home/adebayo/test-findings/test-findings-19-artifacts/final-app-compat/`
- Do not modify the report directly; the final single-writer report pass consumes the handoff.

This task is intentionally low priority. It exists to record deferrals cleanly, not to acquire, build, stage, or run third-party programs during the current critical path.

- [ ] **Step 1: Record external workload deferrals**

Run:

```bash
mkdir -p /home/adebayo/test-findings/test-findings-19-artifacts/final-app-compat
cat > /home/adebayo/test-findings/test-findings-19-artifacts/final-app-compat/handoff.md <<'EOF'
# External Workload Handoff

Status: Deferred
Reason: External/third-party binaries are deprioritized for this pass.
Affected workloads: MicroPython, pdpmake, TCC, BusyBox, and similar externally acquired binaries.
MicroPython prerequisite: compatible static soft-float riscv64-linux-musl toolchain.
Current toolchain note: musl-cross-make may still be installing; do not wait for it in this plan.
Next validation: resume only after shell/job-control probes, PTE endurance, final regression, review workflow, residual-risk decisions, merge, cleanup, report, and notification work are complete, or after the user explicitly reprioritizes external workloads.
EOF
```

Expected:

- The handoff exists and is the only required output for this task in the current pass.
- The final report pass can mark these rows `Deferred:<owner,rationale,next validation>`.

- [ ] **Step 2: Optional inventory after higher-priority gates**

Run only after Tasks 2 through 8 are complete, or if the user explicitly reprioritizes external workloads:

Run:

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
find build rootfs tests thirdparty -type f \
  \( -name '*python*' -o -name '*micropython*' \) \
  2>/dev/null | sort
```

Expected:

- If a workload binary is present, record its exact path in the report.
- If a workload binary is absent, keep the corresponding row status as deferred with the reason `binary absent from local tree/build`.

- [ ] **Step 3: Optional toolchain compatibility check**

Run only after higher-priority gates are complete, or if the user explicitly reprioritizes external workloads:

Run:

```bash
cd /home/adebayo/CSE506
source sbunix-skeleton/tools/sourceme.sh
command -v riscv64-linux-musl-gcc || true
command -v riscv64-linux-gnu-gcc || true
command -v riscv64-unknown-linux-gnu-gcc || true
command -v riscv64-unknown-elf-gcc || true
```

Expected:

- The current repo toolchain provides `riscv64-unknown-elf-gcc`.
- Larger hosted applications usually need a Linux/musl-style RV64 toolchain, not only the freestanding ELF toolchain.
- Prefer static soft-float RV64 binaries built with:

```bash
-static -march=rv64imac_zicsr_zifencei -mabi=lp64
```

Reject candidate binaries that have a dynamic loader:

```bash
file /path/to/candidate
readelf -l /path/to/candidate | rg INTERP || true
readelf -hA /path/to/candidate | rg 'Flags|Tag_RISCV_arch'
```

Expected binary shape:

- RV64 ELF executable.
- Statically linked.
- No `INTERP` program header.
- No hard-float ABI requirement unless SBUnix user ABI support has been audited for it.

This plan intentionally attempts MicroPython only with a static soft-float `riscv64-linux-musl` toolchain. `riscv64-linux-gnu-gcc` and `riscv64-unknown-linux-gnu-gcc` are diagnostic inventory outputs only unless the user explicitly approves a glibc experiment and the candidate binary passes the static/no-`INTERP`/soft-float checks.

If `riscv64-linux-musl-gcc` is absent, do not attempt a large toolchain build inside the repo. Record a MicroPython handoff note with:

- `toolchain missing: riscv64-linux-musl-gcc`
- current free space from `df -h /home/adebayo`
- recommended install prefix `/home/adebayo/toolchains/riscv64-linux-musl`
- whether there is at least 10 GB free for a safe source build

Only build the toolchain if the user explicitly asks for that install step. Otherwise keep MicroPython deferred with owner/rationale/next validation instead of blocking unrelated gates.

- [ ] **Step 4: Deferred reference for staging MicroPython**

Do not run this step in the current pass. Run it only after the user explicitly reprioritizes MicroPython and a compatible binary already exists.

Use `/test/bin` for final validation payloads so the root tarfs does not grow unnecessarily:

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
BUILD_TESTS=1 make -j2 build/kernel.elf build/testfs/.prepared

install -D -m 0755 /path/to/micropython build/testfs/bin/micropython

rm -f build/test-disk.img
BUILD_TESTS=1 make build/test-disk.img
```

Expected:

- The binary appears in the guest as `/test/bin/micropython`.
- If a binary is only needed for the normal root filesystem, stage it into `build/rootfs/bin` and force a tarfs rebuild instead.

- [ ] **Step 5: Deferred reference for building or obtaining MicroPython**

Do not run this step in the current pass. `musl-cross-make` being in progress is not a blocker for Tasks 2 through 8.

Primary source:

- `https://github.com/micropython/micropython`
- Useful docs: `https://docs.micropython.org/en/v1.27.0/develop/gettingstarted.html`

The following is a provisional candidate recipe, not a guaranteed canonical path. Treat it as canonical only after:

- `riscv64-linux-musl-gcc` is installed and passes the static soft-float smoke test from Step 3.
- The MicroPython Unix-port cross-build completes with this exact configuration.
- The resulting binary passes `file` and `readelf` checks for static/no-`INTERP`/soft-float RV64 output.
- The guest smoke test prints `42` under SBUnix.

Candidate build recipe for a minimal hosted interpreter with a RV64 Linux/musl toolchain:

```bash
work=/tmp/sbunix-thirdparty
mkdir -p "$work"
cd "$work"
git clone --recursive https://github.com/micropython/micropython.git
cd micropython

make -C mpy-cross -j"$(nproc)"
cd ports/unix
make submodules
make VARIANT=minimal \
  CROSS_COMPILE=riscv64-linux-musl- \
  CFLAGS_EXTRA="-march=rv64imac_zicsr_zifencei -mabi=lp64" \
  LDFLAGS_EXTRA="-static" \
  -j"$(nproc)"

file build-minimal/micropython
```

Stage:

```bash
install -D -m 0755 build-minimal/micropython \
  /home/adebayo/CSE506/sbunix-skeleton/build/testfs/bin/micropython
```

Guest smoke:

```sh
/test/bin/micropython -c 'print(6 * 7)'
```

Expected:

- Output contains `42`.
- If the build pulls in unsupported hosted-runtime dependencies, leave the row deferred and record the missing dependency or syscall in the report.

- [ ] **Step 6: Keep other third-party workloads deferred**

Do not build, stage, or run pdpmake, TCC, BusyBox, or similar externally acquired binaries in the current pass. They are lower-priority backlog items and must not displace:

- exact shell and job-control probes
- PTE endurance validation
- full final-candidate regression
- review workflow completion
- residual-risk decisions
- merge, cleanup, report, and notification work

Expected:

- The report keeps these workloads deferred with owner/rationale/next validation.
- No time is spent acquiring or compiling these binaries until the rest of `plan-03.md` is complete or the user explicitly reprioritizes them.

- [ ] **Step 7: Deferred reference for running available external workloads**

Do not run this step in the current pass. Run it only after the user explicitly reprioritizes external workloads.

Use one QEMU/tmux session per workload group. Save raw logs under:

```bash
/home/adebayo/test-findings/test-findings-19-artifacts/final-app-compat/
```

Required workload groups:

- Python or MicroPython interpreter smoke.

Expected:

- Passing workload logs include a zero shell status and no fail marker.
- Absent workload logs record the inventory output and the exact absence reason.
- pdpmake, TCC, and BusyBox remain deferred in this pass even if binaries are discovered.

- [ ] **Step 8: Handoff final external-workload dispositions to the report writer**

For the MicroPython workload group:

- Mark `PASS` only when the exact workload ran successfully.
- Mark `Deferred:<owner,rationale,next validation>` when the binary is absent.
- Mark `Blocked:<infrastructure reason>` only for a reproducible environment failure unrelated to kernel behavior.
- While `musl-cross-make` is still installing, use `Deferred:<owner,rationale,next validation>` rather than `Blocked`.

For pdpmake, TCC, and BusyBox:

- Keep their rows deferred.
- Note that they are intentionally lower priority than the remaining non-third-party gates in this plan.

- [ ] **Step 9: Commit only if source, test, or tracked report files change**

Do not commit untracked instruction files. If a commit is needed, choose the prefix from the actual changed scope:

```bash
git status --short
git add <tracked-files-that-changed>
# report-only change:
git commit -m "DOC: Updated final workload validation status"
# focused source fix:
git commit -m "BF: Fixed final workload compatibility issue"
# focused test or harness change:
git commit -m "TEST: Added final workload validation coverage"
```

Expected:

- `DOC:` is used only for report-only updates.
- `BF:` or another valid prefix from `contrib.md`/`CLAUDE.md` is used for source behavior fixes.
- `TEST:` is used for test or harness-only changes.

## Task 2: Exact Shell And Job-Control Probes

**Files:**
- Create artifacts under: `/home/adebayo/test-findings/test-findings-19-artifacts/final-shell/`
- Create handoff: `/home/adebayo/test-findings/test-findings-19-artifacts/final-shell/handoff.md`
- Do not modify `/home/adebayo/test-findings/test-findings-19.md`; Task 8 is the sole report writer.

- [ ] **Step 1: Prepare a fresh guest boot**

Run from `sbunix-skeleton` with the normal toolchain loaded:

```bash
source tools/sourceme.sh
BUILD_TESTS=1 make -j2 build/kernel.elf build/disk.img build/test-disk.img
```

Expected:

- Kernel, root disk, and test disk build without compile errors.

- [ ] **Step 2: Capture exact non-interactive shell probes**

Save one raw log per probe under `/home/adebayo/test-findings/test-findings-19-artifacts/final-shell/`, with the listed filename and exact command sequence. Each probe must be sent through `run_guest` or an equivalent tmux-backed shell harness that records start marker, command output, numeric status, and end marker.

- `noninteractive-pipe-chain.log`: `printf 'beta\nalpha\ngamma\n' | sort | head -n 1`
- `noninteractive-pipe-grep.log`: `printf 'keep\nskip\n' | grep keep`
- `noninteractive-script-args.log`: `printf '#!/bin/sh\necho "$1:$2:$#"\n' > /tmp/args-probe.sh; chmod +x /tmp/args-probe.sh; /tmp/args-probe.sh left right`
- `noninteractive-redir-append.log`: `rm -f /tmp/append-probe.out; echo first >> /tmp/append-probe.out; echo second >> /tmp/append-probe.out; cat /tmp/append-probe.out`
- `noninteractive-eof.log`: `printf 'echo eof-ok\n' | /bin/sh`

Expected:

- Each probe writes a start marker, command output, numeric status, and end marker.
- The numeric status is zero for expected-success cases.
- Expected command output is respectively `alpha`, `keep`, `left:right:2`, both `first` and `second` in order, and `eof-ok`.

- [ ] **Step 3: Capture exact interactive job-control probes**

Run these through a tmux-backed QEMU session and save raw terminal logs:

- `interactive-background-jobs.raw.log`: at `sbush$`, send `echo __START:bg; sleep 10 &; jobs; echo __STATUS:$?; echo __END:bg`, then Enter.
- `interactive-fg.raw.log`: at `sbush$`, send `echo __START:fg; sleep 10 &`, Enter; after the prompt returns, send `fg %1`, Enter; after the foreground command completes and the prompt returns, send `echo __STATUS:$?; echo __END:fg`, Enter.
- `interactive-ctrl-z.raw.log`: at `sbush$`, send `echo __START:ctrlz; cat`, Enter; after `cat` is active, send Ctrl-Z; after the prompt returns, send `jobs; echo __STATUS:$?; fg %1`, Enter; after `cat` resumes, send Ctrl-D; after the prompt returns, send `echo __END:ctrlz`, Enter.
- `interactive-ctrl-d.raw.log`: in a dedicated fresh guest session at `sbush$`, send `echo __START:ctrld`, Enter, then Ctrl-D. Capture whether the shell exits, restarts, or returns to a prompt.

Expected:

- Logs show the shell prompt returning at the expected points.
- Job-control commands report expected state transitions.
- No kernel panic, PTE repair output, or unexpected warning appears in the serial log.
- If `%1` is not accepted by this shell, record the exact failure and use the job id printed by the background launch in a rerun; keep both logs.

- [ ] **Step 4: Handoff exact shell row dispositions to the report writer**

Write `/home/adebayo/test-findings/test-findings-19-artifacts/final-shell/handoff.md` with:

- exact probe log paths
- proposed matrix row disposition changes
- rows that must remain deferred because evidence is still broad local coverage rather than the exact probe

Task 8 is responsible for applying these dispositions to the report.

## Task 3: Deferred PTE Endurance Gate

**Files:**
- Create artifacts under: `/home/adebayo/test-findings/test-findings-19-artifacts/final-pte-endurance/`
- Create handoff: `/home/adebayo/test-findings/test-findings-19-artifacts/final-pte-endurance/handoff.md`
- Do not modify `/home/adebayo/test-findings/test-findings-19.md`; Task 8 is the sole report writer.

- [ ] **Step 1: Build the final candidate with tests**

Run:

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
BUILD_TESTS=1 make -j2 build/kernel.elf build/disk.img build/test-disk.img
```

Expected:

- Build completes successfully.

- [ ] **Step 2: Run the 200-iteration endurance loop**

Use the `run_guest` helper pattern from `plan.md` and run:

```bash
for i in $(seq 1 200); do
  run_guest "pte-$i" "/test/bin/ptextest" 1800
done
```

Expected:

- Every iteration returns zero.
- Every iteration logs the expected pass marker.
- No iteration logs panic, PTE repair output, unexpected warning, timeout, or contamination from a previous run.

- [ ] **Step 3: Run targeted stress checks from the source plan**

Run after the 200-iteration loop:

```bash
run_guest "trapretstress" "/test/bin/trapretstress"
run_guest "membugstest" "/test/bin/membugstest"
run_guest "virtiostress" "/test/bin/virtiostress"
```

Expected:

- Each command returns zero.
- Each log is saved under `/home/adebayo/test-findings/test-findings-19-artifacts/final-pte-endurance/`.
- No log contains panic, PTE repair output, unexpected warning, timeout, or stale output from a previous run.

- [ ] **Step 4: Summarize endurance results**

Create:

```bash
/home/adebayo/test-findings/test-findings-19-artifacts/final-pte-endurance/summary.txt
```

The summary must include:

- Number of iterations attempted.
- Number of iterations passed.
- Number of iterations failed or timed out.
- First failing log path if any failure occurs.
- Targeted stress command status for `trapretstress`, `membugstest`, and `virtiostress`.

Also create `handoff.md` in the same directory with proposed report text and row dispositions for Task 8.

## Task 4: Pre-Merge Full-Candidate Regression

**Files:**
- Create artifacts under: `/home/adebayo/test-findings/test-findings-19-artifacts/final/`
- Create handoff: `/home/adebayo/test-findings/test-findings-19-artifacts/final/pre-merge-regression-handoff.md`
- Do not modify `/home/adebayo/test-findings/test-findings-19.md`; Task 8 is the sole report writer.

This task validates the candidate before merge. The final acceptance run happens after the merge in Task 7 and is the only full-suite evidence that may be used for final close-out.

- [ ] **Step 1: Run the Sortix-style guest suite once for the final candidate**

Run through the guest helper:

```bash
run_guest "final-os-test" "run-os-test" 5400
```

Expected:

- The log is saved under the final artifact directory.
- Any failure is classified against a subsystem in `pre-merge-regression-handoff.md`.

- [ ] **Step 2: Run the Open POSIX-style guest suite once for the final candidate**

Run through the guest helper:

```bash
run_guest "final-open-posix" "run-open-posix-test" 3600
```

Expected:

- The log is saved under the final artifact directory.
- `PASS` output is accepted.
- `UNRESOLVED`, `UNSUPPORTED`, `UNTESTED`, `SKIP`, `FAIL`, and `Test not found` are rejected for exact targeted rows.

- [ ] **Step 3: Run the host two-boot final summary**

Run:

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
set +e
TIMEOUT=900 bash tools/run_tests.sh 2>&1 | tee /home/adebayo/test-findings/test-findings-19-artifacts/final/host-run-tests.log
status=${PIPESTATUS[0]}
set -e
echo "$status" > /home/adebayo/test-findings/test-findings-19-artifacts/final/host-run-tests.status
```

Expected:

- The log contains `SBUnix Test Results`.
- The total test count is nonzero.
- Any timeout or failure is classified in `pre-merge-regression-handoff.md`.

## Task 5: Required Exact Review Workflow

**Files:**
- Create artifacts under: `/home/adebayo/test-findings/test-findings-19-artifacts/reviews/`
- Create handoff: `/home/adebayo/test-findings/test-findings-19-artifacts/reviews/handoff.md`
- Do not modify `/home/adebayo/test-findings/test-findings-19.md`; Task 8 is the sole report writer.

- [ ] **Step 1: Run the branch review command from `plan.md`**

Run the exact review workflow for `rem-plan-02`, using the current branch as the candidate and `os-debug-01` as the comparison base. The review must cover all candidate changes:

```bash
cd /home/adebayo/CSE506
mkdir -p /home/adebayo/test-findings/test-findings-19-artifacts/reviews
candidate_dirty=$(
  {
    git diff --name-only
    git diff --cached --name-only
    git ls-files --others --exclude-standard
  } | awk '$0 !~ /^(AGENTS.md|CLAUDE.md|plan(-[0-9]+)?\.md)$/'
)
if [ -n "$candidate_dirty" ]; then
  printf '%s\n' "$candidate_dirty" > /home/adebayo/test-findings/test-findings-19-artifacts/reviews/rem-plan-02-reviewed-dirty-files.txt
  codex review --base os-debug-01 --uncommitted - <<'PROMPT' > /home/adebayo/test-findings/test-findings-19-artifacts/reviews/rem-plan-02-adversarial.md
Bad-mood senior RV64IMAC SBUnix code review. Prioritize correctness bugs, trap/VM/PTE/ABI regressions, missing tests, and architecture drift. Findings only, severity ordered.
PROMPT
else
  codex review --base os-debug-01 - <<'PROMPT' > /home/adebayo/test-findings/test-findings-19-artifacts/reviews/rem-plan-02-adversarial.md
Bad-mood senior RV64IMAC SBUnix code review. Prioritize correctness bugs, trap/VM/PTE/ABI regressions, missing tests, and architecture drift. Findings only, severity ordered.
PROMPT
fi
```

Expected:

- Review output is saved to:

```bash
/home/adebayo/test-findings/test-findings-19-artifacts/reviews/rem-plan-02-adversarial.md
```
- Dirty worktrees are reviewed with `--uncommitted`.
- The dirty check includes untracked candidate files, while ignoring instruction and plan files that must stay out of staging.
- `rem-plan-02-reviewed-dirty-files.txt` lists the files that triggered the dirty-worktree review path when applicable.
- Clean worktrees are reviewed against the committed branch diff.

- [ ] **Step 2: Resolve accepted Critical or High findings**

If accepted findings exist:

- Implement only the targeted fix.
- Rebuild.
- Rerun the validation that covers the changed subsystem.
- Commit with the required prefix.
- Run the rescue workflow below before merge:

```bash
codex exec --sandbox danger-full-access - <<'PROMPT' > /home/adebayo/test-findings/test-findings-19-artifacts/reviews/rem-plan-02-rescue.md
Apply only fixes for accepted Critical/High findings from /home/adebayo/test-findings/test-findings-19-artifacts/reviews/rem-plan-02-adversarial.md. Do not touch unrelated files. Rebuild and rerun targeted branch validation.
PROMPT
```
- After any rescue or manual blocking fix changes the candidate, rerun the adversarial review and save it as `/home/adebayo/test-findings/test-findings-19-artifacts/reviews/rem-plan-02-adversarial-rerun.md`.

If no accepted Critical or High findings exist:

- Record `PASS/no blocking findings` in `/home/adebayo/test-findings/test-findings-19-artifacts/reviews/handoff.md`.

If the review command cannot run because the tool is unavailable, stop this task, send the ntfy stop notification, and record `Blocked:<review-tool unavailable>` in `/home/adebayo/test-findings/test-findings-19-artifacts/reviews/handoff.md`. Do not merge until either the review succeeds or the user explicitly approves a waiver recorded by Task 8.

- [ ] **Step 3: Enforce the review merge gate**

Expected:

- No unresolved Critical or High findings remain before merge.
- Medium findings are fixed or deferred with OS-kernel rationale in `/home/adebayo/test-findings/test-findings-19-artifacts/reviews/handoff.md`.
- Any user-approved waiver is named explicitly in the review handoff with owner, rationale, and next validation for Task 8 to serialize into the report.
- If any accepted Critical or High finding was fixed, the rerun review has no unresolved Critical or High findings.

## Task 6: Resolve Or Record Residual Risks

**Files:**
- Read: `sbunix-skeleton/kernel/pathres.c`
- Read: `sbunix-skeleton/kernel/ext2.c`
- Modify only if choosing to fix: relevant source files and focused tests
- Create handoff: `/home/adebayo/test-findings/test-findings-19-artifacts/final-residual-risks/handoff.md`
- Do not modify `/home/adebayo/test-findings/test-findings-19.md`; Task 8 is the sole report writer.

- [ ] **Step 1: Decide on dirfd anchoring**

Current residual risk:

- `resolve_path_at()` is dirfd-aware but path-snapshot based.
- Rename or unlink churn can diverge from true inode-anchored directory-fd behavior.

Decision options:

- Fix now by extending the VFS/open-file representation so directory fds retain a stable directory object.
- Defer with explicit rationale because the current plan fixed helper-level dirfd coverage and local validations pass.
- Accept as out of current scope only with owner, rationale, and next validation note: `accepted out of current scope; next validation: none in current plan, track in follow-up backlog if reopened`.

- [ ] **Step 2: Decide on ext2 truncate rollback atomicity**

Current residual risk:

- Ext2 truncate/grow zero-fill is not fully rollback-atomic if a later cache/I/O failure happens after earlier dirty writes.

Decision options:

- Fix now by staging the full truncate operation before dirtying earlier blocks.
- Defer with explicit rationale because local persistence validation passes and the remaining issue is an I/O failure atomicity edge.
- Accept as out of current scope only with owner, rationale, and next validation note: `accepted out of current scope; next validation: none in current plan, track in follow-up backlog if reopened`.

- [ ] **Step 3: Handoff both decisions to the report writer**

Expected:

- Each residual risk has one of: `fixed`, `deferred with owner/rationale/next validation`, or `accepted as out of current scope`.
- Accepted-out-of-scope residual risks are recorded in report section 10 using `AcceptedOutOfScope:<owner,rationale,next validation>`.
- Matrix row `final_status` values remain restricted to `PASS`, `Blocked:<reason>`, or `Deferred:<owner,rationale,next validation>`; do not use `AcceptedOutOfScope` in the 49-row matrix.
- The complete disposition text is written to `/home/adebayo/test-findings/test-findings-19-artifacts/final-residual-risks/handoff.md` for Task 8.

- [ ] **Step 4: Validate any residual-risk source fix**

If either residual risk is marked `fixed`:

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
BUILD_TESTS=1 make -j2 build/kernel.elf build/test-disk.img
```

Expected:

- The fix includes or names a focused test covering the changed behavior.
- The focused test log is saved under `/home/adebayo/test-findings/test-findings-19-artifacts/final-residual-risks/`.
- Ext2 truncate fixes also rerun the two-boot persistence validation before final report closure.
- Dirfd anchoring fixes also rerun these exact dirfd/path validations before final report closure:

```bash
run_guest "dirfd-openat" "/test/bin/os-test/basic/fcntl/openat" 300
run_guest "dirfd-fstatat" "/test/bin/os-test/basic/sys_stat/fstatat" 300
run_guest "dirfd-renameat" "/test/bin/os-test/basic/stdio/renameat" 300
run_guest "dirfd-unlinkat" "/test/bin/os-test/basic/unistd/unlinkat" 300
```

Expected:

- Each dirfd/path command returns zero.
- Each log is saved under `/home/adebayo/test-findings/test-findings-19-artifacts/final-residual-risks/`.
- No log contains panic, PTE repair output, unexpected warning, timeout, or stale output from a previous run.

## Task 7: Merge And Cleanup

**Files:**
- No source files should change unless a final validation issue requires a fix.
- Git refs and worktrees change during cleanup.

- [ ] **Step 1: Confirm clean source scope**

Run:

```bash
cd /home/adebayo/CSE506
git status --short
git status --short -- sbunix-skeleton
sha256sum AGENTS.md CLAUDE.md > /tmp/plan-03-instruction-files.before.sha256
git diff -- AGENTS.md CLAUDE.md > /tmp/plan-03-instruction-files.before.diff
```

Expected:

- No dirty tracked files under `sbunix-skeleton`.
- `AGENTS.md`, `CLAUDE.md`, and plan files remain unstaged unless the user explicitly asks to track a plan file.
- Instruction-file hashes and diffs are captured before merge.

- [ ] **Step 2: Reclaim generated build space before more QEMU-heavy work**

Run:

```bash
df -h /
du -sh /home/adebayo/CSE506/sbunix-skeleton/build \
  /home/adebayo/CSE506-*/sbunix-skeleton/build \
  /tmp/sbunix-thirdparty 2>/dev/null | sort -h
```

Delete only generated build outputs, never source, reports, instruction files, or worktree metadata:

```bash
rm -rf /home/adebayo/CSE506/sbunix-skeleton/build
rm -rf /home/adebayo/CSE506-*/sbunix-skeleton/build
if pgrep -af 'musl-cross-make|riscv64-linux-musl|sbunix-thirdparty' >/tmp/plan-03-active-external-builds.txt; then
  cat /tmp/plan-03-active-external-builds.txt
  echo "active external/toolchain build detected; skipping /tmp/sbunix-thirdparty cleanup"
else
  rm -rf /tmp/sbunix-thirdparty
fi
df -h /
```

Expected:

- Free space is high enough for `BUILD_TESTS=1` rebuilds and QEMU runs.
- Deleted directories are regenerable build outputs only.
- Active `musl-cross-make` or external/toolchain install directories are not interrupted or removed.
- If free space remains below 2 GB, stop and send the ntfy stop notification before doing more builds.

- [ ] **Step 3: Merge `rem-plan-02` into `os-debug-01` after final gates pass**

Run only after Tasks 1 through 6 are complete or explicitly deferred in the report:

```bash
cd /home/adebayo/CSE506
if git show-ref --verify --quiet refs/heads/os-debug-01; then
  git switch os-debug-01
else
  git switch -c os-debug-01 --track origin/os-debug-01
fi
git merge --no-ff rem-plan-02 -m "MERGE: Merged rem-plan-02"
sha256sum -c /tmp/plan-03-instruction-files.before.sha256
git diff --exit-code -- AGENTS.md CLAUDE.md
```

Expected:

- Merge succeeds.
- `CLAUDE.md` and `AGENTS.md` are not staged or committed.
- Instruction-file hashes match the pre-merge capture, and the instruction-file diff remains unchanged.

- [ ] **Step 4: Run a post-merge compile or validation smoke**

Run:

```bash
cd /home/adebayo/CSE506/sbunix-skeleton
source tools/sourceme.sh
BUILD_TESTS=1 make -j2 build/kernel.elf
```

Expected:

- Kernel builds successfully after merge.

- [ ] **Step 5: Handle post-merge smoke failures**

If the post-merge compile or smoke fails:

- Save the failing command log under `/home/adebayo/test-findings/test-findings-19-artifacts/final/post-merge-smoke-failure.log`.
- Do not clean worktrees or delete branches.
- Either fix forward on `os-debug-01`, or revert the merge and create `rem-plan-02-redo` from current `os-debug-01`, cherry-pick verified commits, fix, revalidate, and merge the redo branch.
- Do not re-merge an already reverted branch unchanged.
- Send the ntfy stop notification if execution stops at this failure.

- [ ] **Step 6: Run post-merge full final-candidate regression**

Repeat the Task 4 full-suite and host two-boot commands after the merge while checked out on `os-debug-01`. Save post-merge logs under:

```bash
/home/adebayo/test-findings/test-findings-19-artifacts/final/post-merge/
```

Expected:

- This post-merge run is the final acceptance evidence.
- The pre-merge Task 4 run is treated only as candidate confidence evidence.
- Any post-merge timeout or failure is classified in `/home/adebayo/test-findings/test-findings-19-artifacts/final/post-merge/handoff.md` before Task 8 updates the report.

- [ ] **Step 7: Clean merged worktrees and branches**

Only after final QEMU leak checks pass:

```bash
git worktree list
```

Remove only clean, merged worktrees. Delete merged branches with:

```bash
git branch -d <merged-branch>
```

Expected:

- No active worktree with unreviewed changes is removed.
- Cleanup result is recorded in `/home/adebayo/test-findings/test-findings-19-artifacts/final/cleanup-handoff.md` for Task 8.

## Task 8: Final Report And Notification

**Files:**
- Modify: `/home/adebayo/test-findings/test-findings-19.md`
- Create artifacts under: `/home/adebayo/test-findings/test-findings-19-artifacts/final/`

- [ ] **Step 0: Collect all handoff notes**

Read the handoff notes produced by non-final tasks:

```bash
find /home/adebayo/test-findings/test-findings-19-artifacts \
  -path '*/handoff.md' -o -name '*-handoff.md' | sort
```

Expected:

- Every completed non-final task that proposes report text has a handoff file.
- Task 8 is the only step that applies those handoffs to `/home/adebayo/test-findings/test-findings-19.md`.
- Missing handoff files for skipped/deferred tasks are acceptable only when the final report explicitly records that task as deferred or not run with rationale.

- [ ] **Step 1: Ensure the report has the required 13 sections**

Run:

```bash
rg -n "^## " /home/adebayo/test-findings/test-findings-19.md
```

Expected sections:

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

- [ ] **Step 2: Ensure every matrix row has a final disposition**

Run:

```bash
awk '/^\| [0-9][0-9]_|^\| sh_/{c++} END{print c}' /home/adebayo/test-findings/test-findings-19.md
```

Expected:

- Output is exactly `49`.
- Every final status is `PASS`, `Blocked:<reason>`, or `Deferred:<owner,rationale,next validation>`.
- Empty report sections must contain exactly `None` or `Blocked:<reason>`; do not leave blank sections and do not append duplicate sections.
- Residual-risk acceptance uses the narrative section 10 form `AcceptedOutOfScope:<owner,rationale,next validation>` and must not be used as a matrix row `final_status`.

- [ ] **Step 3: Send final ntfy notification**

Run:

```bash
notif_log=/home/adebayo/test-findings/test-findings-19-artifacts/final/notification.log
if /home/adebayo/.agents/skills/ntfy-notify/scripts/ntfy_send.sh \
  --topic ab-mac \
  --title "os-debug-01" \
  "final status: /home/adebayo/test-findings/test-findings-19.md" \
  >"$notif_log" 2>&1; then
  echo "success" > /home/adebayo/test-findings/test-findings-19-artifacts/final/notification.status
else
  echo "failure" > /home/adebayo/test-findings/test-findings-19-artifacts/final/notification.status
fi
```

Expected:

- Notification status is written.
- The report notification section records success or failure.

## Parallelization Plan

Run these tracks in parallel if resources allow:

- Worker A: external/app workload deferral handoff only; no third-party builds, staging, or guest runs unless the user explicitly reprioritizes this track.
- Worker B: exact shell and job-control probes.
- Worker C: PTE endurance loop.
- Worker D: review workflow and residual-risk decision prep.

Do not run more than two QEMU-heavy jobs at once. Keep final full-suite regression single-candidate and non-concurrent.

Parallel workers write artifacts and handoff notes only. The final report update is serialized through the single report writer described in `Execution Ownership Rules`.

## Commit Rules

- Use the prefix convention from `contrib.md` and `CLAUDE.md`.
- Keep commit messages 8-12 words after the prefix where practical.
- Never stage or commit `CLAUDE.md` or `AGENTS.md`.
- Do not push.
- Do not include co-author tags.
- Do not use prohibited evaluation-source names or prohibited wording in commit messages, comments, documentation, or code.

---

## Spec Review - Round 1

---

## Spec Alignment Review

### Issues

- **[HIGH] [RESOLVED]** The plan drops the `trapretstress`, `membugstest`, and `virtiostress` runs that `plan.md` includes in the final targeted validation set - this leaves the final gate incomplete for trap-return, memory, and virtio regressions - add those `run_guest` commands and their artifact capture to Task 3 or Task 4, or explicitly document that they were already completed with matching logs and report entries.
- **[MEDIUM] [RESOLVED]** Task 5 makes the adversarial review workflow optional even though `plan.md` treats post-change Codex review as part of the required merge gate - skipping it removes the only explicit high-signal review step before merge - make the review mandatory and require no unresolved Critical/High findings before merge, or record a concrete approved waiver in the report.
- **[MEDIUM] [RESOLVED]** The merge/cleanup and final-report tasks omit two `plan.md` safeguards: instruction-file hash/diff verification before and after merge, and the explicit empty-section formatting rule for the 13-section report - without those, an implementation agent can merge with silent instruction-file drift or produce a report that fails downstream parsing - add the hash checks to Task 7 and state the `None` / `Blocked:<reason>` rule in Task 8.

### Summary

The plan is close to the stated close-out workflow, but it is not complete enough to hand off as written. It still misses part of the required final validation set, weakens the mandatory review gate into an optional one, and omits a couple of merge/report safeguards that the source plan expects. Those gaps should be fixed before execution.

## Execution Review - Round 1

---

## Execution Quality Review

### Issues

- **[HIGH] [RESOLVED]** Parallel worker tracks all write to the same report file without a single-writer rule - Tasks 1, 2, 3, 4, 5, 6, and 8 all allow `/home/adebayo/test-findings/test-findings-19.md` to change, while the parallelization plan explicitly encourages concurrent workers, so concurrent report edits can clobber row dispositions or leave the final section inconsistent - Assign one worker or one serialized integration step as the sole writer for the report, and keep parallel workers limited to artifacts plus handoff notes.
- **[MEDIUM] [RESOLVED]** The shell validation probes are named but not fully specified - Task 2 asks for "exact non-interactive shell probes" and "exact interactive job-control probes," but it never defines the exact command lines or keystroke sequence for each probe, so two implementers can produce different evidence for the same row - Enumerate the exact commands and input sequence for each probe and bind them to specific artifact filenames or report rows.

### Summary

The plan is mostly executable, but it needs a stricter ownership model for report updates and concrete probe definitions before implementation starts. Without those changes, parallel execution can corrupt the final report and the shell-validation evidence can drift across workers.

---

## Spec Review - Round 2

---

## Spec Alignment Review

### Issues

- **[HIGH] [RESOLVED]** Task 4 runs the full guest suites and host two-boot summary before the merge step, so the "final candidate" being validated is still the pre-merge branch instead of the merged `os-debug-01` state required by `plan.md` - a merge can introduce regressions or invalidate the tested artifacts, which means the close-out gate is not actually satisfied - move the full regression to after Task 7 merge/cleanup or rerun it on the merged branch before final report and notification updates.
- **[MEDIUM] [RESOLVED]** Task 5 copies the review workflow without the `--uncommitted` flag that `plan.md` requires when reviewing dirty branch changes - if the branch still has uncommitted work, the review will miss part of the candidate and the merge gate can pass on an incomplete diff - either add `--uncommitted` or state explicitly that the branch must be clean and only committed changes are in scope.

### Summary

The plan is close, but it still misorders the final regression gate and under-specifies the review invocation. As written, it validates the wrong candidate state and can skip uncommitted changes during the required review, so those two points need to be fixed before implementation begins.

## Execution Review - Round 2

---

## Execution Quality Review

### Issues

- **[MEDIUM] [RESOLVED]** Task 1 hardcodes a `DOC:` commit message even though it allows source and test fixes - if validation uncovers a narrow fix, `DOC: Updated final workload validation status` will mislabel the commit and can violate the required prefix convention - make the commit message conditional on the actual change type, or restrict this step to report-only edits.
- **[MEDIUM] [RESOLVED]** Task 5 does not state whether the review candidate must be clean or dirty, and its `codex review` invocation omits the `--uncommitted` mode used in `plan.md` for dirty worktrees - that can cause the adversarial review to miss uncommitted fixes that are still part of the candidate - state the branch-state precondition explicitly and use the dirty-worktree review form when needed.
- **[MEDIUM] [RESOLVED]** Task 7 omits the post-merge failure recovery path from `plan.md` - if the merge succeeds but the smoke fails, the implementer is left without a prescribed fix-forward/revert sequence, which can strand the workflow half-integrated - add the explicit recovery rule before the post-merge smoke step.

### Summary

The plan is close to executable, but it still has a few instruction-level gaps that can misroute commits, weaken the required review coverage, and leave merge failures without a recovery path. Tightening those three spots will make the handoff reliable.

---

## Spec Review - Round 3

---

## Spec Alignment Review

### Issues

- **[HIGH] [RESOLVED]** Task 5 runs the adversarial review but omits the explicit `codex exec` rescue step that `plan.md` defines as part of the required review workflow - accepted Critical/High findings can be left without the guarded fix-forward process the plan expects, which weakens the merge gate and makes the review output harder to act on safely - add a rescue substep that runs the prescribed `codex exec --sandbox danger-full-access` command against the review artifact, then require its completion and rerun validation before merge.
- **[MEDIUM] [RESOLVED]** Task 6 only offers "fix now" or "defer" decisions for the residual risks, even though the task text and `plan.md` also allow "accepted as out of current scope" - an implementer has no canonical way to record a deliberate acceptance, so the report can end up misclassifying a valid decision or forcing an unnecessary deferment - add an explicit third outcome for each risk: "accepted as out of current scope," with the report wording and next-validation note spelled out.

### Summary

The plan is still not fully handoff-ready. The review workflow is missing the rescue half that the source plan requires, and the residual-risk section does not give the implementer a complete set of decision states. Both should be corrected before execution starts.

## Execution Review - Round 3

---

## Execution Quality Review

### Issues

- **[MEDIUM] [RESOLVED]** The MicroPython gate is narrower than the inventory step and can defer a workload that is still buildable - Task 1a checks `riscv64-linux-gnu-gcc` and `riscv64-unknown-linux-gnu-gcc`, but Task 1c only permits `riscv64-linux-musl-gcc`, so a host with a usable Linux cross-toolchain but no musl toolchain will be marked deferred even though the workload could still be validated - either accept the first available hosted RV64 cross-compiler from the inventory list and parameterize the build, or state explicitly that the workload is intentionally deferred unless musl is present.
- **[MEDIUM] [RESOLVED]** The required adversarial review is not rerun after a blocking fix is applied - Task 5 tells the agent to fix accepted Critical or High findings, rebuild, and rerun subsystem validation, but it never forces a fresh `codex review` on the updated candidate before the merge gate, so the plan can merge on stale review results - add a mandatory review rerun after any accepted blocking fix and before Step 3.
- **[MEDIUM] [RESOLVED]** Residual-risk fixes can change source without any explicit revalidation step - Task 6 allows either risk to be fixed now, but it only says to reflect the decision in the report, so a worker could land a source change and finalize the report without a rebuild or focused regression - require a rebuild plus a targeted validation for whichever residual risk is marked `fixed` before the report is closed.

### Summary

The plan is executable in broad shape, but it still leaves a few avoidable holes in the close-out workflow. The MicroPython gate can be deferred unnecessarily, the review gate can become stale after a fix, and residual-risk source changes are not paired with a required validation step. Those should be tightened before implementation starts.

---

## Spec Review - Current Run Round 1

---

## Spec Alignment Review

### Issues

- **[MEDIUM] [RESOLVED]** The review-gate dirty/clean check only looks at tracked diffs (`git diff` and `git diff --cached`), so a candidate that adds new source or test files can be misclassified as "clean" and reviewed without `--uncommitted` - that lets untracked implementation changes bypass the mandatory pre-merge adversarial review - switch the predicate to detect any working-tree change, including untracked files, or always run the `--uncommitted` review before commit-time gating.

### Summary

The plan is mostly aligned with the close-out workflow, but the review gate still has one real hole: a branch that introduces new files can skip the dirty-worktree review path and leave candidate changes outside the mandatory review. Fix that predicate before execution so the adversarial review covers the full candidate every time.

## Execution Review - Current Run Round 1

---

## Execution Quality Review

### Issues

- **[HIGH] [RESOLVED]** The mandatory `codex review` gate can miss untracked candidate files - the branch-state check only looks at tracked diff and cached diff, so a new source or test file that is still untracked will take the clean-tree path and skip `--uncommitted`, leaving part of the candidate outside the adversarial review - include untracked files in the dirty/clean check, for example with `git ls-files --others --exclude-standard`, before deciding which review form to run.
- **[MEDIUM] [RESOLVED]** Task 6 tells the implementer to rerun an "exact dirfd/path validation" if the resolver risk is fixed, but it never names the concrete command or test binary - that leaves the acceptance check underspecified and makes it easy to validate the wrong path behavior - name the exact guest probe to rerun, ideally the specific `openat`/`fstatat`/`renameat`-style regression that covers the changed resolver.
- **[MEDIUM] [RESOLVED]** The residual-risk disposition grammar is incomplete for the plan's own decision set - Task 6 allows `accepted as out of current scope`, but Task 8 only permits `PASS`, `Blocked:<reason>`, or `Deferred:<owner,rationale,next validation>` for final statuses, so an accepted risk has no canonical report encoding - add an explicit report form for accepted-out-of-scope decisions or say which narrative section must carry that disposition.

### Summary

The plan is close to executable, but it still has two execution-quality gaps and one reporting mismatch that matter in practice. The review gate can skip untracked files, the dirfd fix lacks a concrete validation command, and the report grammar does not fully cover the plan's own residual-risk outcomes. Those should be tightened before anyone starts the close-out run.

---

## Spec Review - Current Run Round 2

---

## Spec Alignment Review

### Issues

- **[HIGH] [RESOLVED]** The plan contradicts its own report-write ownership rules: the execution ownership section says only the final integration/report pass may edit `test-findings-19.md`, but Tasks 2, 3, 4, 5, 6, and 8 also tell earlier workers to update that same report - this matters because parallel workers can race on the canonical report and an implementer cannot tell which step is allowed to mutate the source of truth - make the report single-writer by moving every report edit into Task 8, or explicitly exempt the earlier tasks from the ownership rule and define one serialization path.
- **[MEDIUM] [RESOLVED]** The deferred MicroPython path is still a speculative build recipe: it assumes a working `riscv64-linux-musl` cross toolchain and a specific unix-port `make` invocation without proving that the port supports that cross-build or that the output binary is usable on SBUnix - this matters because if external workloads are reprioritized later, the worker may burn time on a dead-end build command - mark the commands as a provisional candidate recipe and require a successful toolchain/build probe before treating them as the canonical MicroPython path.

### Summary

The plan is mostly aligned with the remaining validation, merge, and report-closeout work, but it is not hand-off ready as written. The report-update ownership model needs to be made consistent before execution, and the MicroPython deferment path needs clearer validation boundaries so it does not turn into an unproven build mandate later.

## Execution Review - Current Run Round 2

---

## Execution Quality Review

### Issues

- **[HIGH] [RESOLVED]** Multiple tasks still authorize direct edits to `/home/adebayo/test-findings/test-findings-19.md` even though the ownership rules say only the final integration/report pass may write the report - Task 2 Step 4, Task 4, Task 5, and Task 6 all tell the executor to update the report, so parallel workers or midstream steps can clobber dispositions and leave the final serialization inconsistent - Restrict every non-final task to artifacts and handoff notes only, and move all report mutation into one serialized report-writer step.
- **[MEDIUM] [RESOLVED]** The review-tool failure path is underspecified because it requires a handoff note without naming one - Task 5 says to record `Blocked:<review-tool unavailable>` in the handoff note if `codex review` cannot run, but it never defines the file path, so the stop workflow has no canonical place to land - Add an explicit handoff file under the reviews artifact directory and point the failure path at that file.

### Summary

The plan is close, but it still mixes report writes into steps that are supposed to hand off work and not mutate shared state, and its review-tool failure branch has no concrete file target. Tighten those two points before execution so the report stays single-writer and the blocker path is usable.
