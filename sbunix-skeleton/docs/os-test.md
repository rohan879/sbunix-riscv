# os-test: Sortix POSIX Compliance Test Suite

This directory integrates the [Sortix os-test](https://sortix.org/os-test/) POSIX compliance
test suite for cross-compilation and execution on SBUnix.

## Quick Start

```bash
# Build kernel and tests
BUILD_TESTS=1 make

# Run QEMU
make qemu

# Inside SBUnix shell (tests do NOT run automatically on boot):
run-tests                                # Run SBUnix regression tests
run-os-test                              # Run sample os-test POSIX tests
run-os-test include/stdarg/va_arg       # Run specific os-test
```

## Building

Tests are built when `BUILD_TESTS=1` is set:

```bash
BUILD_TESTS=1 make
```

This compiles:
1. All regular test binaries from `tests/*/`
2. The Sortix os-test suite from `tests/thirdparty/sortix/os-test/`

Tests that fail to compile (due to missing POSIX features in SBUnix libc) are
skipped with a warning - no stubs are created.

## Build Outputs

| Path | Description |
|------|-------------|
| `build/rootfs/bin/os-test/` | Compiled test binaries (included in tarfs) |
| `build/os-test-out/sbunix/build-summary.txt` | Build summary: counts of compiled/skipped |
| `build/os-test-out/sbunix/compile-logs/<suite>/<test>.log` | Detailed compilation logs for ALL tests |

## Running Tests

### In QEMU

```bash
# Start QEMU
make qemu

# Run sample tests (checks basic functionality)
run-os-test

# Run specific test
run-os-test include/string/strlen
run-os-test process/fork
```

## Analyzing Compilation Failures

The build system creates detailed logs for every test compilation (success and failure).

### Quick Analysis

```bash
# Use the analysis script for comprehensive reporting
./tools/analyze-os-test-failures

# Show most common missing headers
./tools/analyze-os-test-failures headers

# Show most common undeclared symbols  
./tools/analyze-os-test-failures symbols

# Show success rate by test suite
./tools/analyze-os-test-failures suite
```

### Manual Analysis

```bash
# See summary
cat build/os-test-out/sbunix/build-summary.txt

# See detailed compilation log for any test (success or failure)
cat build/os-test-out/sbunix/compile-logs/include/pthread/pthread_create.log

# Find all tests that failed due to missing headers
grep -r "missing_header" build/os-test-out/sbunix/compile-logs/ | wc -l

# Find all successful compilations
grep -r "COMPILED SUCCESSFULLY" build/os-test-out/sbunix/compile-logs/ | wc -l

# Most common missing headers
grep -r "fatal error:" build/os-test-out/sbunix/compile-logs/ | \
  sed 's/.*fatal error: //' | sed 's/: No such file.*//' | \
  sort | uniq -c | sort -nr | head -10
```

## Test Suites

The os-test suite contains these categories (from `misc/suites.list`):

| Suite | Description |
|-------|-------------|
| `basic` | Basic C functionality |
| `include` | Header file compliance (largest suite) |
| `limits` | System limits |
| `io` | I/O operations |
| `malloc` | Memory allocation |
| `namespace` | Symbol namespace |
| `paths` | Path handling |
| `process` | Process management |
| `pty` | Pseudo-terminals |
| `signal` | Signal handling |
| `stdio` | Standard I/O |
| `udp` | UDP networking |

## Coverage Statistics

As of the last build:
- **~1,100 tests** compile successfully (~21% of total)
- **~4,200 tests** skipped due to missing features

Most skipped tests require:
- `<pthread.h>` - Threading (not implemented)
- `<sys/socket.h>` - Networking (not implemented)
- `<sys/mman.h>` - Full mmap support (partial)
- Various POSIX extensions

## Implementation Details

### Build System

The build is controlled by `tools/os-test.mk`, invoked from the main Makefile:

```make
# In Makefile
ifdef BUILD_TESTS
$(OS_TEST_MARKER): build/libc/crt.S.o build/libc.a
    $(MAKE) -f tools/os-test.mk SBUNIX_ROOT=$(CURDIR) install
    @touch $@
endif
```

The os-test Makefile:
1. Iterates through each suite directory
2. Attempts to compile each `.c` file
3. Logs failures with categorized reasons (missing_header, undeclared_symbol, etc.)
4. Copies successful binaries to `build/rootfs/bin/os-test/`

### Guest Runner

`tools/guest-run-os-test` is copied to `/bin/run-os-test` in the rootfs and provides:
- Sample test execution for quick validation
- Individual test execution with `run-os-test <path>`
- Pass/fail counting

## Troubleshooting

### "Test not found" error

The test failed to compile. Check the error log:
```bash
cat build/os-test-out/sbunix/<suite>/<test>.err
```

Common reasons:
- `missing_header`: Required header not in SBUnix libc
- `undeclared_symbol`: Function/constant not defined
- `link_error`: Compiles but can't link (missing implementation)

### Rebuilding

```bash
# Clean and rebuild
make clean
BUILD_TESTS=1 make
```

## Adding POSIX Support

To make more tests pass:

1. Check which tests fail and why:
   ```bash
   grep -l "missing_header" build/os-test-out/sbunix/*/*.err
   ```

2. Add the missing header/function to `libc/`

3. Rebuild:
   ```bash
   BUILD_TESTS=1 make
   ```

4. Verify the test now compiles and passes

## References

- [Sortix os-test](https://sortix.org/os-test/) - Original test suite
- [POSIX.1-2017](https://pubs.opengroup.org/onlinepubs/9699919799/) - POSIX specification
