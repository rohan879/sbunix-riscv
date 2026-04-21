# Plan 03 Audit

This audit exists because `plan-04.md` explicitly requires a `plan-03` reality check before the third-party workload wave starts.

## Summary

`plan-03.md` correctly finished the CR07 stabilization and validation work, but it also deliberately deferred the exact workloads that `plan-04.md` elevates to must-fix status. The current branch therefore starts Wave 1 with a strong kernel baseline and an incomplete third-party/runtime integration story.

## Audit Table

| Area | What `plan-03.md` said or implied | What actually landed | Preserve / revise |
| --- | --- | --- | --- |
| Core CR07 remediation | Finish remaining source fixes, validation, merge, and reporting | Landed on `os-auxillary` via `rem-safety-abi`, `rem-pte`, `rem-signal-time`, and `rem-plan-02`; see `/home/adebayo/test-findings/test-findings-19.md` sections 4, 8, and 11 | Preserve |
| Third-party workloads | Defer MicroPython, pdpmake, TCC, BusyBox until after higher-priority gates | All exact external workload rows remained deferred in findings 19; no deterministic guest promotion path was added for the vendored binaries | Revise |
| Toolchain prerequisite | Treat `riscv64-linux-musl-gcc` as optional inventory unless reprioritized | The toolchain now exists at `/home/adebayo/toolchains/riscv64-linux-musl/bin/riscv64-linux-musl-gcc`, so Wave 0 no longer needs bootstrap work | Revise |
| Guest staging | No current-pass staging or runtime layout work for third-party payloads | Repo still lacked deterministic promotion of `thirdparty/bin/*`, `/usr/lib/tcc`, `/bin/make`, and BusyBox applets into guest images at the start of this pass | Revise |
| `/proc` validation | Local `procfstest` was considered good enough for that pass | `/proc` implementation exists and local coverage passes, but the exact external row was still not proven | Preserve the code, revise the validation status |
| Shell and job control | Exact probes were promoted to required gates | Exact shell probes were captured and mostly passed in findings 19 artifacts | Preserve |
| PTE endurance and broad regression | Run late, record mixed-suite results honestly | Completed and documented; broad suites still expose residual POSIX gaps but not CR07 regressions | Preserve |
| Review / reporting workflow | Final report and notification were required | Completed for findings 19, including notification | Preserve |

## Wrong or incomplete assumptions in `plan-03.md`

| Assumption | Why it was incomplete |
| --- | --- |
| External workloads could stay deferred without affecting the next execution plan | `plan-04.md` makes the exact third-party rows first-class deliverables, so deferral is no longer an acceptable terminal state |
| Toolchain setup might still be a blocker | Host inspection now shows the musl toolchain and staged binaries already exist; the blocker shifted from build acquisition to guest promotion and runtime compatibility |
| The current BusyBox/TCC/pdpmake/MicroPython assets were mainly a future concern | The vendored RV64 static binaries are present and host-runnable now, so the missing work is guest layout, validation, and kernel/runtime seam closure |

## Keep vs discard

Keep:

- The CR07 fixes and their validation evidence from findings 19
- The exact shell/job-control probe methodology
- The PTE fail-fast and endurance evidence
- The instruction not to treat broad-suite reruns as substitutes for issue-specific proof

Discard or replace:

- The idea that third-party workloads should remain deferred in this pass
- Any staging path that depends on ad hoc manual copying into `build/rootfs` or `build/testfs`
- Any assumption that `/proc` is fully done without exact-row validation

## Next execution consequences

1. Wave 0 only needs toolchain verification, not toolchain bootstrap.
2. The first code wave should prioritize deterministic guest staging plus environment layout.
3. After staging, the kernel/runtime fixes should be driven by real workload failures, not inferred blockers.
