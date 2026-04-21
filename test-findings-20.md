# Test Findings 20

Date: 2026-04-21

## Scope

This pass executed the actionable parts of `plan-04.md` that were already in flight:

- finish third-party guest staging into the normal boot image
- unblock BusyBox startup on external musl binaries
- implement the narrow `vfork`-style clone path needed by `pdpmake`
- replace the staged guest `tcc` with a source-rebuilt path-clean binary
- fix the remaining RISC-V `tcc` linker failure and rerun targeted guest validations
- verify the staged workload binaries from a nested `sbush -c` shell

I did not run the single full regression pass yet. The targeted third-party validations in this report are green.

## Implemented Work

- Added deterministic third-party staging via `sbunix-skeleton/tools/stage-thirdparty.sh`, wired into image prep so `micropython`, `tcc`, `pdpmake`, BusyBox, `/bin/make`, `/usr/lib/tcc`, and `/usr/lib/busybox` are staged into the guest image.
- Updated `bin/init/init.c` and `rootfs/etc/rc` so `PATH` includes `/usr/lib/busybox/bin`.
- Added minimal `prctl(PR_SET_NAME/PR_GET_NAME)` support for external musl userlands.
- Added Linux-style exec auxv entries plus page-aligned initial `brk`, which fixed BusyBox startup and musl guard-page setup.
- Implemented a narrow `CLONE_VM|CLONE_VFORK|SIGCHLD` path in `sys_clone`, including parent blocking until child `execve` or exit, and release hooks for normal exit and signal-kill paths.
- Rebuilt the vendored `tcc` from source so it bakes in only `/usr/lib/tcc`-relative include/lib/crt paths, then replaced the staged `libtcc1.a` with the matching rebuild output.
- Removed the staging-time binary patching path from `tools/stage-thirdparty.sh`; staging now copies the rebuilt payload directly.
- Fixed TinyCC's RISC-V linker backend so it handles the plain `R_RISCV_HI20`, `R_RISCV_LO12_I`, and `R_RISCV_LO12_S` relocations used by the musl `crt1.o` startup path.
- Added a local patch artifact at `sbunix-skeleton/thirdparty/patches/tcc-riscv64-hi20-lo12.patch` so the rebuild recipe is reproducible.
- Replaced TinyCC's hardcoded RISC-V `lp64d` metadata with SBUnix's soft-float `rv64imac/lp64` shape, covering both emitted ELF flags and target predefines.
- Added a local patch artifact at `sbunix-skeleton/thirdparty/patches/tcc-riscv64-soft-float.patch` so the soft-float metadata fix is reproducible.
- Updated the local TCC build notes with the working bootstrap recipe (`unset CC AR`, static cross `c2str.exe`, `riscv64-libtcc1-usegcc=yes`, implicit `-static` link default, soft-float patch).
- Patched `/proc/self/...` handling to use `current_proc` directly so short-lived external commands can read `/proc/self/status` and `/proc/self/stat`.

## Targeted Results

### Passes

- BusyBox suite: pass
  - `busybox-suite-vforkfix.serial.log`
  - direct applet, symlink applet, and PATH-based invocation all worked
- `pdpmake` shell-backed recipe: pass
  - `pd.serial.log`
  - recipe `all:; /mnt/pdpmake-cmd` now executes and prints `pdpmake-ok`
- `/proc/self/status` and `/proc/self/stat`: pass after procfs self-path fix
  - `procfix.manual.capture.log`
- `/proc/version` and `/proc/1/status`: pass
  - `procdiag.manual.capture.log`
- `tcc -vv`: pass
  - `tv.serial.log`
  - `subsh-verify.guest.log`
  - resolved paths now show:
    - `install: /usr/lib/tcc`
    - `include: /usr/lib/tcc/include`
    - `libraries: /usr/lib/tcc/lib`
    - `crt: /usr/lib/tcc/lib`
- Guest `tcc` compile+run of a tiny C program: pass
  - `subsh-verify.guest.log`
  - `sbush -c 'printf "int main(void) { return 0; }\n" >/mnt/hello.c && tcc /mnt/hello.c -o /mnt/hello && /mnt/hello'`
  - nested-shell run completed and returned to the prompt
- Host `qemu-riscv64` run of the same rebuilt `tcc` against the same staged sysroot: pass
  - `tcc-final.host.log`
  - plain `tcc hello.c -o hello` now emits a static binary with no `INTERP`
- Host `qemu-riscv64` output ABI metadata after the soft-float patch: pass
  - `tcc-softfloat.host.log`
  - plain `tcc hello.c -o hello` now emits a soft-float ELF (`Flags: 0x0`)
  - TinyCC predefines now advertise `__riscv_float_abi_soft` instead of `__riscv_float_abi_double`
- Nested `sbush -c` verification for staged workload binaries: pass
  - `subsh-verify.guest.log`
  - verified:
    - `micropython -c 'print(42)'`
    - `busybox echo ...`
    - `/usr/lib/busybox/bin/echo ...`
    - `pdpmake -f /mnt/pd.mk`
    - `/bin/make -f /mnt/pd.mk`
    - `tcc /mnt/hello.c -o /mnt/hello && /mnt/hello`

## Root Cause Notes

- `pdpmake` was blocked by the kernel rejecting the exact `vfork`-style clone shape it uses (`CLONE_VM|CLONE_VFORK|SIGCHLD`). Once that path was allowed and the parent-blocking semantics were added, the recipe execution path worked.
- BusyBox startup had two separate issues:
  - missing `prctl` handling
  - unaligned initial `brk`, which broke musl's `MAP_FIXED` guard-page setup
- The `tcc` path problem was partly staging and partly binary configuration:
  - staging now places the sysroot where the guest expects it
  - the vendored binary is now rebuilt with guest-visible `/usr/lib/tcc` paths only
  - the remaining blocker was not SBUnix path handling: the same rebuilt binary failed under Linux `qemu-riscv64` with the same relocation diagnostics against the same sysroot payload
  - the specific root cause was TinyCC's RISC-V linker backend missing handlers for absolute `R_RISCV_HI20`, `R_RISCV_LO12_I`, and `R_RISCV_LO12_S` relocations
  - those relocations come from the musl `crt1.o` startup object, so link failed before guest runtime behavior was relevant
  - after adding those relocation handlers and making `-static` implicit for this guest-targeted build, the same `hello.c` compile works both on the host under `qemu-riscv64` and inside SBUnix
- TinyCC's RISC-V metadata also assumed `lp64d` unconditionally:
  - `tccelf.c` hardcoded `EF_RISCV_FLOAT_ABI_DOUBLE`
  - `riscv64-gen.c` hardcoded `__riscv_float_abi_double`, `__riscv_flen 64`, `__riscv_fdiv`, and `__riscv_fsqrt`
  - for SBUnix, the pragmatic fix was to mark output as soft-float and expose `__riscv_float_abi_soft`, which matches the `rv64imac/lp64` target used to build the guest workloads
- `/proc/self/...` failed for short-lived external commands because the self-path logic depended on a proc-table lookup that was not reliable for the live caller. Using `current_proc` directly fixed the self view.

## Commands Run

- Rebuild:
  - `cd sbunix-skeleton && source tools/sourceme.sh && make -j4`
- Portable TCC rebuild:
  - `cd /home/adebayo/src/third-party/tinycc && unset CC AR`
  - `patch -p1 < /home/adebayo/CSE506/sbunix-skeleton/thirdparty/patches/tcc-riscv64-hi20-lo12.patch`
  - `patch -p1 < /home/adebayo/CSE506/sbunix-skeleton/thirdparty/patches/tcc-riscv64-soft-float.patch`
  - `./configure --prefix=/usr --tccdir=/usr/lib/tcc --cpu=riscv64 --targetos=Linux --cross-prefix='riscv64-linux-musl-' --cc='gcc -static -no-pie' --ar='ar' --triplet=riscv64-linux-musl --config-musl --disable-rpath --sysincludepaths='{B}/include' --libpaths='{B}/lib' --crtprefix='{B}/lib' --tcc-switches='-static' --extra-cflags='-Os -fno-pie -march=rv64imac -mabi=lp64' --extra-ldflags='-static -no-pie'`
  - `make -j"$(nproc)" riscv64-libtcc1-usegcc=yes`
- Targeted guest smokes:
  - BusyBox suite
  - `/proc/version`, `/proc/1/status`, `/proc/self/status`, `/proc/self/stat`
  - `pdpmake` script recipe via `/bin/make`
  - `tcc -vv`
  - `tcc /mnt/hello.c -o /mnt/hello`
- Host separation check:
  - `qemu-riscv64 sbunix-skeleton/thirdparty/bin/tcc -B sbunix-skeleton/thirdparty/tcc-sysroot /tmp/tcc-host-hello.c -o /tmp/tcc-host-hello`
- Nested shell verification:
  - outer prompt `sbush`
  - inner commands via `sbush -c '...'`

## Artifacts

- Directory:
  - `/home/adebayo/test-findings/test-findings-20-artifacts/targeted-rootfs`
- Most relevant logs:
  - `busybox-suite-vforkfix.serial.log`
  - `pd.serial.log`
  - `tv.serial.log`
  - `tccdiag.manual.capture.log`
  - `tcc-final.config.log`
  - `tcc-final.build.log`
  - `tcc-final.host.log`
  - `tcc-softfloat.build.log`
  - `tcc-softfloat.host.log`
  - `tcc-softfloat.guest.log`
  - `subsh-verify.guest.log`
  - `procdiag.manual.capture.log`
  - `procfix.manual.capture.log`

## Status

- BusyBox: usable in the guest image
- `pdpmake`: usable for shell-backed recipes
- procfs self-view: usable for external commands
- `tcc`: rebuilt portably from source, links `hello.c`, the resulting guest binary runs, and emitted ELF metadata now matches SBUnix's soft-float target
- Nested `sbush -c` workload launch: usable for all staged third-party binaries validated here
- Full regression: not run in this pass
