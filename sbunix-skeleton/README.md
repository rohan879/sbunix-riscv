# SBUnix Skeleton

This repository contains the SBUnix kernel, userspace, and local test tooling.

## Build

Set up the toolchain path first:

```bash
source tools/sourceme.sh
```

Build the kernel:

```bash
make
```

Build a test-enabled image:

```bash
make BUILD_TESTS=1 build/kernel.elf build/disk.img
```

## Test Workflow

The test system is guest-first and is organized into staged local runners under `tools/`.

### 1. Smoke

Build the kernel and disk image and refresh the test coverage matrix:

```bash
bash tools/ci_smoke.sh
```

### 2. Focused Regressions

Run the focused on-target regressions for the highest-risk areas:

```bash
bash tools/ci_focused.sh
```

This stage currently runs:

- `tools/run_fdiotest_target.sh`
- `tools/run_lazytest_target.sh`
- `tools/run_sh_exit_code_target.sh`
- `tools/run_core_regressions_target.sh`
- `tools/run_shell_e2e_target.sh`

### 3. Full Guest Suite

The full test suite can be run using:

```bash
make test [time (sec)]
```

Run the integrated guest suite and the two-boot persistence flow:

```bash
bash tools/ci_full.sh
```

This stage runs:

- `tools/run_tests.sh`
- `tools/run_persist_flow.sh`

### 4. Third-Party Inventory

Check whether external payloads required for some test categories are present:

```bash
bash tools/ci_thirdparty.sh all
```

This reports inventory for:

- Python
- `make+TCC`
- BusyBox

These categories are tracked in the suite, but they remain blocked until the corresponding binaries are placed in `thirdparty/`.

## Coverage Matrix

The test-to-suite mapping is generated from:

- `tools/test_manifest.json`

Regenerate the matrix with:

```bash
python3 tools/render_test_coverage.py
```

The generated report is written to:

- `docs/review/2026-03-13-test-coverage-matrix.md`

## Live Output

The host-side runners stream live guest serial output by default. The main integrated runner is:

```bash
bash tools/run_tests.sh
```

Focused target proofs also stream live output.

## Current Limits

- Interactive shell categories are now exercised by `tools/ci_interactive_smoke.sh`, and the current local results are tracked in the coverage matrix.
- Third-party categories are tracked, but execution is blocked until the external binaries are supplied in `thirdparty/`.
