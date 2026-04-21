# Third-Party Workload Install Notes

Date: 2026-04-20

Important:

- Do not export `CC`, `CXX`, `AR`, `STRIP`, or `CROSS_COMPILE` for `riscv64-linux-musl` before building the toolchain itself.
- `musl-cross-make` must use the host compiler to build host-side tools during bootstrap.
- Export the `riscv64-linux-musl-*` variables only after the toolchain has been installed successfully.

After the toolchain is installed, use:

```bash
export PATH=/home/adebayo/toolchains/riscv64-linux-musl/bin:$PATH
export CROSS_COMPILE=riscv64-linux-musl-
export CC=${CROSS_COMPILE}gcc
export AR=${CROSS_COMPILE}ar
export STRIP=${CROSS_COMPILE}strip
```

Target binary properties for SBUnix workload trials:

- RV64 executable.
- Static link.
- No `INTERP` program header.
- Soft-float ABI: `lp64`, not `lp64d`.
- ISA close to `rv64imac`.

Use this check on every staged binary:

```bash
file ./candidate-binary
readelf -l ./candidate-binary | grep INTERP || true
readelf -hA ./candidate-binary | grep -E 'Type:|Machine:|Flags:|Tag_RISCV_arch'
```

The toolchain itself is not included in the per-project space estimates below.

## riscv64-linux-musl Install

Use `musl-cross-make` for this. Local Ubuntu packages expose `riscv64-linux-gnu`, but I did not find a packaged `riscv64-linux-musl` compiler locally, and the glibc toolchain is not the right default target for this SBUnix/MicroPython experiment.

Compatibility note:

- If GCC reports `unsupported ISA subset 'z'` for `rv64imac_zicsr_zifencei`, use `rv64imac` instead.
- Older toolchains treat CSR and `fence.i` support as part of the base `I` extension and do not accept the newer explicit `zicsr` / `zifencei` spelling.
- The `ABI requires '-march=rv64'` message is a follow-on parse failure. `rv64imac` already satisfies the RV64 requirement for `-mabi=lp64`.

Recommended install location outside the repo:

```bash
mkdir -p /home/adebayo/src /home/adebayo/toolchains
cd /home/adebayo/src
git clone https://github.com/richfelker/musl-cross-make.git
cd musl-cross-make
```

Before running `make`, clear any existing target-toolchain exports from the current shell:

```bash
unset CROSS_COMPILE CC CXX AR AS LD STRIP RANLIB
hash -r
```

Create `config.mak`:

```make
TARGET = riscv64-linux-musl
OUTPUT = /home/adebayo/toolchains/riscv64-linux-musl
GCC_VER = 13.3.0

COMMON_CONFIG += --disable-nls
COMMON_CONFIG += CFLAGS="-O2" CXXFLAGS="-O2" LDFLAGS="-s"

GCC_CONFIG += --with-arch=rv64imac
GCC_CONFIG += --with-abi=lp64
GCC_CONFIG += --disable-multilib
GCC_CONFIG += --enable-languages=c
```

Why set `GCC_VER` explicitly:

- The local `musl-cross-make` default currently resolves to `GCC_VER = 9.4.0`.
- On this RISC-V target, that older GCC can fail while linking `libgcc_s.so` with an error like:

```text
relocation R_RISCV_JAL against `__udivdi3' which may bind externally can not be used when making a shared object; recompile with -fPIC
```

- A newer GCC release avoids this older `libgcc` RISC-V issue.

Build and install:

```bash
make -j1
make install

export PATH=/home/adebayo/toolchains/riscv64-linux-musl/bin:$PATH
export CROSS_COMPILE=riscv64-linux-musl-
export CC=${CROSS_COMPILE}gcc
export AR=${CROSS_COMPILE}ar
export STRIP=${CROSS_COMPILE}strip
```

If you previously built with the older default GCC and saw the `R_RISCV_JAL` / `__udivdi3` failure, rebuild from a clean state before retrying:

```bash
cd /home/adebayo/src/musl-cross-make
rm -rf build/local/riscv64-linux-musl/obj_gcc
rm -f sources/gcc-9.4.0.tar.xz
make -j1
make install
```

Then run the compatibility smoke test:

```bash
cat >/tmp/sbunix-toolchain-smoke.c <<'EOF'
int main(void) { return 0; }
EOF

riscv64-linux-musl-gcc -static -no-pie -fno-pie \
  -march=rv64imac -mabi=lp64 \
  -o /tmp/sbunix-toolchain-smoke /tmp/sbunix-toolchain-smoke.c

file /tmp/sbunix-toolchain-smoke
readelf -l /tmp/sbunix-toolchain-smoke | grep INTERP || true
readelf -hA /tmp/sbunix-toolchain-smoke | grep -E 'Type:|Machine:|Flags:|Tag_RISCV_arch'
```

Expected result: static RV64 executable, soft-float ABI, no `INTERP`.

Space estimate for `musl-cross-make` with C-only GCC:

- Source/download cache: roughly 300-600 MB.
- Peak build directory: roughly 5-8 GB.
- Installed toolchain: roughly 400 MB-1.2 GB.
- After deleting build sources/cache, long-term cost should be around 0.5-1.2 GB.

## Dependency Checklist

Shared host dependencies:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  ca-certificates \
  file \
  git \
  make \
  perl \
  pkg-config \
  python3 \
  qemu-user \
  xz-utils
```

Optional host dependencies:

```bash
sudo apt-get install -y \
  bc \
  bison \
  flex \
  libffi-dev \
  libncurses-dev
```

Notes:

- `qemu-user` is only needed for host-side smoke tests of cross-built RV64 binaries.
- `libncurses-dev` is only needed for interactive BusyBox `menuconfig`.
- `libffi-dev` is useful for a fuller host-side MicroPython Unix build, but the RV64 minimal port should avoid depending on host libffi.
- `bison`, `flex`, and `bc` are conservative build-tool additions for C projects that generate parsers/config data. They may not be used by the minimal procedures below, but installing them avoids common build interruptions.

Required target toolchain dependency for all four workload builds:

```text
riscv64-linux-musl-gcc
riscv64-linux-musl-ar
riscv64-linux-musl-strip
riscv64-linux-musl-readelf or compatible readelf
riscv64-linux-musl-objcopy
static musl sysroot for RV64 lp64
```

The existing `riscv64-unknown-elf-gcc` is not sufficient for these third-party Unix-style workloads. It is useful for SBUnix-native freestanding binaries, but these workloads need a hosted Linux/musl-style libc and startup files.

Per-tool dependency summary:

| Tool | Required dependencies | Extra dependency notes |
|------|-----------------------|------------------------|
| MicroPython | `git`, `make`, host `gcc`, `python3`, `riscv64-linux-musl-gcc` | `mpy-cross` is built for the host first; the final `ports/unix` binary is cross-built for RV64. |
| TCC | `git`, `make`, host shell tools, `riscv64-linux-musl-gcc` | Compile-and-link tests inside SBUnix also need guest-visible musl headers, `crt*.o`, `libc.a`, libgcc support files, and `libtcc1.a`. |
| pdpmake | `git`, `make`, `riscv64-linux-musl-gcc` | No large external dependency tree; host-side upstream tests need a way to execute the RV64 binary, such as `qemu-riscv64`. |
| BusyBox | `git`, `make`, host `gcc`, `perl`, `riscv64-linux-musl-gcc` | `libncurses-dev` is needed only for `menuconfig`; `scripts/config` can avoid interactive config. |

## Recommended Workspace

Use a scratch directory outside the repository:

```bash
mkdir -p /home/adebayo/src/third-party
mkdir -p /home/adebayo/third-party-stage/bin
cd /home/adebayo/src/third-party
```

After each workload is staged, remove its source/build directory if disk space is tight.

## MicroPython

Priority: current third-party focus.

Source:

- Upstream repository: https://github.com/micropython/micropython
- Build docs: https://docs.micropython.org/en/v1.27.0/develop/gettingstarted.html

MicroPython should be compiled from source. Do not use board firmware downloads; those are not SBUnix guest executables.

Procedure:

```bash
cd /home/adebayo/src/third-party
git clone --recursive https://github.com/micropython/micropython.git
cd micropython

# Host tool used during the build. This must run on the host, not RV64.
make -C mpy-cross -j"$(nproc)"

cd ports/unix
make submodules

make VARIANT=minimal \
  CROSS_COMPILE="${CROSS_COMPILE}" \
  CFLAGS_EXTRA="-Os -static -fno-pie -march=rv64imac -mabi=lp64" \
  LDFLAGS_EXTRA="-static -no-pie" \
  -j"$(nproc)"

${STRIP} build-minimal/micropython || true
cp build-minimal/micropython /home/adebayo/third-party-stage/bin/micropython
```

Expected artifact:

```text
ports/unix/build-minimal/micropython
/home/adebayo/third-party-stage/bin/micropython
```

Supporting runtime/build files:

- For a basic guest-side smoke test, none beyond the `micropython` binary itself.
- For fuller guest-side use, also stage any `.py` scripts, modules, or data files you want to execute inside SBUnix.

Space estimate:

- Source checkout with submodules: about 100-300 MB.
- Build tree: about 100-500 MB, depending on enabled modules and debug output.
- Staged binary: usually low single-digit MB after stripping.
- Conservative peak while building: 500 MB-1 GB.
- Long-term footprint after cleanup: staged binary only, usually under 10 MB.

Runtime caveats:

- The Unix port expects Unix-like syscalls and libc behavior. A static binary can still fail at runtime if SBUnix lacks a syscall or implements different edge behavior.
- Build `mpy-cross` for the host first; only the final Unix-port executable is cross-built for RV64.

Cleanup:

```bash
rm -rf /home/adebayo/src/third-party/micropython
```

## TCC

Priority: lower than MicroPython and core SBUnix issues.

Source:

- Current upstream repository: https://repo.or.cz/tinycc.git
- GitHub mirror: https://github.com/TinyCC/tinycc
- Original project page: https://bellard.org/tcc/

TCC has two separate goals:

1. Run a `tcc` binary inside SBUnix.
2. Use that guest `tcc` to compile and link a tiny C program inside SBUnix.

The first is just a static RV64 executable. The second also needs guest-visible headers, startup objects, and libraries. Without those, `tcc hello.c -o hello` will fail even if the `tcc` binary starts.

Procedure for a static RV64 TCC binary:

```bash
cd /home/adebayo/src/third-party
git clone https://repo.or.cz/tinycc.git
cd tinycc
git checkout mob

./configure \
  --prefix=/usr \
  --cpu=riscv64 \
  --targetos=Linux \
  --cross-prefix="${CROSS_COMPILE}" \
  --config-musl \
  --disable-rpath \
  --extra-cflags="-Os -fno-pie -march=rv64imac -mabi=lp64" \
  --extra-ldflags="-static -no-pie"

make -j"$(nproc)"
${STRIP} tcc || true
cp tcc /home/adebayo/third-party-stage/bin/tcc
```

If the build produces a prefixed executable instead of `tcc`, locate it before staging:

```bash
find . -maxdepth 2 -type f -perm -111 -name '*tcc*' -print
```

Procedure for the compile-and-run tiny C program test:

```bash
mkdir -p /home/adebayo/third-party-stage/tcc-sysroot/include
mkdir -p /home/adebayo/third-party-stage/tcc-sysroot/lib

# Copy the musl headers, crt objects, libc.a, libgcc pieces, and libtcc1.a
# that match the exact riscv64-linux-musl toolchain used to build TCC.
# Exact paths vary by toolchain installation.
```

Required guest-visible support files for in-SBUnix compilation:

- C headers: at minimum `stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, and the musl `bits/` plus arch-specific headers they include.
- Startup objects: `crt1.o`, `crti.o`, `crtn.o`.
- Static C runtime: `libc.a`.
- TCC runtime support: `libtcc1.a`.
- Compiler support library: usually `libgcc.a` or the equivalent helper objects from the exact RV64 musl toolchain used to build TCC.

Minimal staged layout:

```text
/home/adebayo/third-party-stage/tcc-sysroot/
  include/
  lib/crt1.o
  lib/crti.o
  lib/crtn.o
  lib/libc.a
  lib/libgcc.a
  lib/libtcc1.a
```

Without this staged sysroot, `tcc hello.c -o hello` may start but fail during header lookup or final link.

Tiny test source:

```c
int main(void) {
  return 0;
}
```

Expected artifacts:

```text
tcc
libtcc1.a
guest-visible include/ and lib/ material, if compiling inside SBUnix
/home/adebayo/third-party-stage/bin/tcc
```

Supporting runtime/build files:

- For basic execution of the `tcc` binary, no extra staged files are required beyond the binary itself.
- For guest-side compilation and linking, stage the guest-visible sysroot described above: headers, `crt1.o`, `crti.o`, `crtn.o`, `libc.a`, `libgcc.a`, and `libtcc1.a`.

Space estimate:

- Source checkout: about 10-30 MB.
- Build tree: about 30-150 MB.
- Staged `tcc` binary and TCC support library: usually under 5-15 MB.
- Guest compile sysroot subset: about 20-150 MB, depending on how much of musl/GCC support is copied.
- Conservative peak while building: 150-300 MB.
- Long-term footprint:
  - binary-only run test: under 15 MB;
  - compile-and-link test: 30-200 MB.

Runtime caveats:

- TCC RISC-V support exists in the current development branch, but this is still the riskiest workload here.
- `tcc -run` may need executable anonymous mappings and other behavior SBUnix might not support yet.
- For a compile-and-run test, prefer explicit compile then execute:

```bash
tcc hello.c -o hello
./hello
```

Cleanup:

```bash
rm -rf /home/adebayo/src/third-party/tinycc
```

## pdpmake

Priority: lower than MicroPython and core SBUnix issues.

Source:

- Upstream repository: https://github.com/rmyorston/pdpmake
- Project page: https://frippery.org/make/
- Manual page: https://manpages.opensuse.org/Tumbleweed/pdpmake/pdpmake.1

Upstream does not define targets named `basic` or `build`. The closest mapping is:

- Basic target: compile the executable only.
- Build/full target: compile, run upstream tests on the host if useful, and install into a staging directory.

Basic build procedure:

```bash
cd /home/adebayo/src/third-party
git clone https://github.com/rmyorston/pdpmake.git
cd pdpmake

make \
  CC="${CC}" \
  CFLAGS="-Os -fno-pie -march=rv64imac -mabi=lp64" \
  LDFLAGS="-static -no-pie"

${STRIP} make || true
cp make /home/adebayo/third-party-stage/bin/pdpmake
```

Direct compiler bootstrap alternative:

```bash
${CC} -Os -static -no-pie -fno-pie \
  -march=rv64imac -mabi=lp64 \
  -o make *.c
cp make /home/adebayo/third-party-stage/bin/pdpmake
```

Full/staged build procedure:

```bash
make clean
make \
  CC="${CC}" \
  CFLAGS="-Os -fno-pie -march=rv64imac -mabi=lp64" \
  LDFLAGS="-static -no-pie"

mkdir -p /home/adebayo/third-party-stage/bin
cp make /home/adebayo/third-party-stage/bin/pdpmake
```

Do not run the upstream `make test` with the cross-built RV64 binary on the host unless using `qemu-riscv64` or an equivalent user-mode emulator.

Expected artifact:

```text
make
/home/adebayo/third-party-stage/bin/pdpmake
```

Supporting runtime/build files:

- For basic execution, none beyond the `pdpmake` binary itself.
- For meaningful guest-side validation, stage one or more test `Makefile`s plus any input files they reference.

Space estimate:

- Source checkout: under 1 MB.
- Build tree: under 5 MB.
- Staged binary: tens to hundreds of KB, depending on static libc overhead.
- Conservative peak while building: under 10 MB.
- Long-term footprint after cleanup: under 1 MB for the staged binary.

Runtime caveats:

- `-j` is accepted by pdpmake but ignored.
- Extensions are enabled by default; use `.POSIX`, `--posix`, or `PDPMAKE_POSIXLY_CORRECT=1` for strict POSIX behavior.
- As with every external static binary, runtime depends on SBUnix syscall coverage.

Cleanup:

```bash
rm -rf /home/adebayo/src/third-party/pdpmake
```

## BusyBox

Priority: lower than MicroPython and core SBUnix issues.

Source:

- Project page: https://busybox.net/
- Source downloads: https://busybox.net/downloads/
- Source browser: https://git.busybox.net/busybox/
- Build FAQ: https://busybox.net/FAQ.html

Procedure:

```bash
cd /home/adebayo/src/third-party
git clone https://git.busybox.net/busybox/
cd busybox

make CROSS_COMPILE="${CROSS_COMPILE}" defconfig

# Force static output without using menuconfig.
scripts/config -e STATIC

make \
  CROSS_COMPILE="${CROSS_COMPILE}" \
  EXTRA_CFLAGS="-Os -fno-pie -march=rv64imac -mabi=lp64" \
  LDFLAGS="--static -no-pie" \
  -j"$(nproc)"

${STRIP} busybox || true
cp busybox /home/adebayo/third-party-stage/bin/busybox
```

If `scripts/config` is unavailable or fails, use:

```bash
make CROSS_COMPILE="${CROSS_COMPILE}" menuconfig
```

Then enable:

```text
Settings -> Build Options -> Build static binary (no shared libs)
```

Minimal fs-utility-focused configuration:

```bash
make CROSS_COMPILE="${CROSS_COMPILE}" allnoconfig
make CROSS_COMPILE="${CROSS_COMPILE}" menuconfig
```

Enable only applets needed for the SBUnix fs utility trial, such as `cat`, `cp`, `dd`, `echo`, `ln`, `ls`, `mkdir`, `mv`, `pwd`, `rm`, `rmdir`, `stat`, `sync`, `touch`, and `true`.

Optional staged applet tree:

```bash
make CROSS_COMPILE="${CROSS_COMPILE}" \
  CONFIG_PREFIX=/home/adebayo/third-party-stage/busybox-root \
  install
```

Expected artifacts:

```text
busybox
/home/adebayo/third-party-stage/bin/busybox
/home/adebayo/third-party-stage/busybox-root/...
```

Supporting runtime/build files:

- For direct invocation like `busybox ls`, none beyond the `busybox` binary itself.
- For normal command-name invocation, stage applet symlinks or an installed applet tree so commands like `ls`, `cp`, and `rm` resolve to the BusyBox binary.
- Some applets may also expect SBUnix runtime facilities such as `/dev`, `/proc`, or specific config files, but those are guest-environment concerns rather than additional build artifacts.

Space estimate:

- Source checkout or release tarball unpacked: about 10-40 MB.
- Defconfig build tree: about 50-200 MB.
- Static BusyBox binary: commonly around 1-3 MB for a broad config; smaller for a minimal applet set.
- Installed applet tree: usually low single-digit MB because most entries are links to one binary.
- Conservative peak while building: 100-300 MB.
- Long-term footprint:
  - single binary only: about 1-3 MB;
  - binary plus applet links: about 2-10 MB.

Runtime caveats:

- BusyBox is Linux-oriented. Many applets will start only if the needed Linux syscalls, `/dev`, `/proc`, terminal behavior, and filesystem operations exist.
- For SBUnix, start with simple fs applets before shells, networking, process tools, or mount-related commands.
- Avoid dynamic BusyBox builds; SBUnix does not provide a Linux dynamic loader.

Cleanup:

```bash
rm -rf /home/adebayo/src/third-party/busybox
```

## Suggested Execution Order

1. Confirm `riscv64-linux-musl-gcc` produces static `lp64` RV64 binaries.
2. Build and stage pdpmake first because it is tiny and low-risk.
3. Build and stage a minimal BusyBox fs-utility set.
4. Build and stage MicroPython minimal Unix port.
5. Leave TCC for last because compile-and-link testing needs a guest-visible compiler sysroot, not only a `tcc` executable.

## Aggregate Space Planning

With the toolchain already installed and projects built one at a time:

```text
pdpmake:       < 10 MB peak
BusyBox:       100-300 MB peak
MicroPython:   500 MB-1 GB peak
TCC:           150-300 MB peak for binary-only, up to 300-500 MB with sysroot staging
```

If all source/build trees are kept simultaneously, reserve about 1.5-2.5 GB beyond the installed toolchain. If disk is tight, build one project, copy only the final artifact into `/home/adebayo/third-party-stage`, then delete the source/build tree before moving to the next project.
