# Wave 0 Evidence Matrix

This matrix correlates the ranked `plan-04.md` workstreams with concrete local evidence so the implementation order is driven by observed failures, existing deferrals, or explicit workload requirements.

| Priority | Workstream | Evidence source | Observed fact | Immediate implication |
| --- | --- | --- | --- | --- |
| 1 | Third-party staging and runtime layout | `/home/adebayo/test-findings/test-findings-19.md`, `sbunix-skeleton/Makefile`, `sbunix-skeleton/thirdparty/Makefile` | Findings 19 deferred all exact third-party rows, and the repo only staged BusyBox manually via `make thirdparty` | Add deterministic guest promotion for the vendored binaries and support files first |
| 2 | Spawn/exec ABI compatibility | `plan-04.md`, `sbunix-skeleton/kernel/syscall_process.c`, vendored pdpmake behavior expectations | `sys_clone()` still rejects every `CLONE_VM` shape with `-EINVAL`, while pdpmake’s documented path depends on a vfork-like `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)` flow | Expect a kernel change once pdpmake is staged and the real failure is captured |
| 3 | TCC private-dir and sysroot pathing | `strings thirdparty/bin/tcc`, `sbunix-skeleton/thirdparty/tcc-sysroot`, `docs/3rd-party-install.md` | Vendored TCC already knows about `/usr/lib/tcc`, but the guest did not yet receive that runtime tree automatically | Stage `/usr/lib/tcc` before touching TCC itself |
| 4 | `/proc` exact-row proof | Findings 19 row 13, `tests/procfstest/procfstest.c`, `kernel/procfs.c` | Local procfs coverage passes, but the exact external row remained deferred | Re-validate `/proc` with the staged guest and only patch it if the exact workload breaks |
| 5 | BusyBox and wider POSIX seams | Findings 19 deferred BusyBox rows; `test-expectations.md` lists BusyBox fs/text rows as never-passed | BusyBox was never staged into the guest images in a way the exact rows could exercise | Stage BusyBox binary plus applet tree, then let the workload surface the next seam |
| 6 | MicroPython | Findings 19 deferred row 07; host smoke on vendored binary succeeds | The binary artifact is good; the missing proof is guest execution and filesystem interaction | Staging plus guest smoke should happen before any rebuild discussion |

## Supporting observations

| Topic | Evidence | Result |
| --- | --- | --- |
| Host musl toolchain | `command -v riscv64-linux-musl-gcc` | Present at `/home/adebayo/toolchains/riscv64-linux-musl/bin/riscv64-linux-musl-gcc` |
| Disk headroom | `df -h /home/adebayo` | About 38 GB free, so local build and image work are not currently space-blocked |
| Vendored binary shape | `file` and `readelf -l` on `thirdparty/bin/{micropython,tcc,pdpmake,busybox}` | All are static RV64 soft-float executables with no `PT_INTERP` |
| Host smoke: MicroPython | `qemu-riscv64 thirdparty/bin/micropython -c 'print(6*7)'` | Passes and prints `42` |
| Host smoke: BusyBox | `qemu-riscv64 thirdparty/bin/busybox echo host-busybox-ok` | Passes |
| Host smoke: pdpmake | `qemu-riscv64 thirdparty/bin/pdpmake -f /tmp/pdpmake-host.mk` | Passes on a trivial host-side makefile |
| Host smoke: TCC basic startup | `qemu-riscv64 thirdparty/bin/tcc -v` | Passes and reports a current riscv64 Linux build |

## Execution order justified by evidence

1. Fix guest promotion and environment layout.
2. Run exact staged workload smokes to expose real runtime failures.
3. Patch only the subsystems that those workloads actually trip.
