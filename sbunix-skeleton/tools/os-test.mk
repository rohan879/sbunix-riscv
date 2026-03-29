# tools/os-test.mk - Cross-compile Sortix os-test for SBUnix/RISC-V
#
# This Makefile cross-compiles the Sortix os-test suite for SBUnix.
# Tests that fail to compile are logged but skipped (no stubs created).
# This shows which POSIX features SBUnix is missing.
#
# Usage from sbunix-skeleton/:
#   make -f tools/os-test.mk SBUNIX_ROOT=$(pwd) install
#
# Or via the main Makefile with BUILD_TESTS=1

SBUNIX_ROOT ?= $(CURDIR)/..
OS_TEST_SRC := $(SBUNIX_ROOT)/tests/thirdparty/sortix/os-test
CROSS := riscv64-unknown-elf-

CC := $(CROSS)gcc
OS := SBUnix
OUT_OS := sbunix
OUT_DIRECTORY := $(SBUNIX_ROOT)/build/os-test-out/$(OUT_OS)
NO_SHARED := 1

# Paths to SBUnix libc
LIBC_INCLUDE := $(SBUNIX_ROOT)/libc/include
CRT_OBJ := $(SBUNIX_ROOT)/build/libc/crt.S.o
LIBC_A := $(SBUNIX_ROOT)/build/libc.a

# Cross-compilation flags matching main Makefile
CFLAGS := -gdwarf-4 -Wall -Os -ffreestanding -fno-builtin -nostdlib -nostdinc \
          -isystem $(shell $(CC) -print-file-name=include) \
          -I$(LIBC_INCLUDE) \
          -mcmodel=medany -march=rv64imac_zicsr_zifencei -mabi=lp64 \
          -ffunction-sections -fdata-sections

# Preprocessor flags for POSIX feature test macros
CPPFLAGS := -D_GNU_SOURCE -D_BSD_SOURCE -D_ALL_SOURCE -D_DEFAULT_SOURCE

# We can't use -Werror since many tests intentionally test for missing features
CFLAGS_WARN := -Wall -Wextra -Werror=implicit-function-declaration

# Linker flags - CRT must be first, libc last
LDFLAGS := $(CRT_OBJ) $(LIBC_A) -Wl,--gc-sections -nostdlib

# Install target: copy compiled binaries to build/testfs
OS_TEST_DEST ?= $(SBUNIX_ROOT)/build/testfs/bin/os-test
SUITE_LIST := basic include limits io malloc namespace paths process pty signal stdio udp

# Summary and log files for build results
SUMMARY_FILE := $(OUT_DIRECTORY)/build-summary.txt
COMPILE_LOG_DIR := $(OUT_DIRECTORY)/compile-logs
MANIFEST_FILE := $(OUT_DIRECTORY)/test-manifest.txt

# Suppress log files for tests that compile and link without warnings
OS_TEST_SUPPRESS_OK_LOGS ?= 1

# Enable parallel builds
MAKEFLAGS += --output-sync=recurse

# Find all test source files and create targets
ALL_SOURCES := $(shell find $(OS_TEST_SRC) -name '*.c' 2>/dev/null | sort)
COMPILE_TARGETS := $(patsubst $(OS_TEST_SRC)/%.c,$(OS_TEST_DEST)/%,$(ALL_SOURCES))

.PHONY: all install clean setup

all: setup $(COMPILE_TARGETS)
	@echo ""
	@echo "=== os-test build complete ==="
	@# Generate manifest from actually compiled binaries (avoids parallel append issues)
	@echo "# Test manifest for SBUnix os-test" > $(MANIFEST_FILE)
	@echo "# Format: /test/bin/os-test/path/to/test" >> $(MANIFEST_FILE)
	@find $(OS_TEST_DEST) -type f ! -name '*.failed' -executable | sort | \
		sed 's|$(SBUNIX_ROOT)/build/testfs|/test|' >> $(MANIFEST_FILE)
	@# Generate summary of failed test outcomes (.out files)
	@find $(OUT_DIRECTORY) -name '*.out' -type f | sort | while read f; do \
		rel=$$(echo "$$f" | sed 's|$(OUT_DIRECTORY)/||;s|\.out$$||'); \
		outcome=$$(cat "$$f"); \
		echo "$$rel: $$outcome" >> $(SUMMARY_FILE); \
	done
	@ok_count=$$(find $(OS_TEST_DEST) -type f ! -name '*.failed' | wc -l); \
	total_count=$$(find $(OS_TEST_SRC) -name '*.c' | wc -l); \
	skip_count=$$((total_count - ok_count)); \
	out_count=$$(find $(OUT_DIRECTORY) -name '*.out' | wc -l); \
	echo "Compiled: $$ok_count tests"; \
	echo "Skipped:  $$skip_count tests (missing features)"; \
	echo ".out files generated: $$out_count"; \
	echo ""; \
	echo "Binaries installed to: $(OS_TEST_DEST)"; \
	echo "Compilation logs: $(COMPILE_LOG_DIR)/<suite>/<test>.log"; \
	echo "Test manifest: $(MANIFEST_FILE) ($$ok_count entries)"

setup:
	@echo "=== Building os-test for SBUnix (RISC-V) ==="
	@echo "CC=$(CC)"
	@echo "SBUNIX_ROOT=$(SBUNIX_ROOT)"
	@echo "OS_TEST_SRC=$(OS_TEST_SRC)"
	@echo ""
	@mkdir -p $(OUT_DIRECTORY)
	@mkdir -p $(COMPILE_LOG_DIR)
	@mkdir -p $(OS_TEST_DEST)
	@echo "os-test build summary for SBUnix" > $(SUMMARY_FILE)
	@echo "================================" >> $(SUMMARY_FILE)
	@echo "" >> $(SUMMARY_FILE)

install: all

# Pattern rule for compiling individual tests
$(OS_TEST_DEST)/%: $(OS_TEST_SRC)/%.c | setup
	@relpath=$*; \
	dir=$$(dirname $$relpath); \
	name=$$(basename $$relpath); \
	suite=$$(echo $$dir | cut -d'/' -f1); \
	mkdir -p $(OUT_DIRECTORY)/$$dir; \
	mkdir -p $(OS_TEST_DEST)/$$dir; \
	mkdir -p $(COMPILE_LOG_DIR)/$$dir; \
	outbin=$(OUT_DIRECTORY)/$$relpath; \
	compile_log=$(COMPILE_LOG_DIR)/$$relpath.log; \
	rm -f $$outbin $(OUT_DIRECTORY)/$$relpath.o; \
	_tmpc=$$(mktemp); \
	_tmpl=$$(mktemp); \
	_keep_log=0; \
	if $(CC) $(CFLAGS) $(CFLAGS_WARN) $(CPPFLAGS) -c $< -o $(OUT_DIRECTORY)/$$relpath.o 2>$$_tmpc; then \
		if [ -s $$_tmpc ]; then _keep_log=1; fi; \
		cat $$_tmpc > $$compile_log; \
		if $(CC) $(LDFLAGS) $(OUT_DIRECTORY)/$$relpath.o -o $$outbin 2>$$_tmpl; then \
			if [ -s $$_tmpl ]; then _keep_log=1; fi; \
			cat $$_tmpl >> $$compile_log; \
			if [ $$_keep_log -eq 0 ] && [ "$(OS_TEST_SUPPRESS_OK_LOGS)" = "1" ]; then \
				rm -f $$compile_log; \
			fi; \
			rm -f $(OUT_DIRECTORY)/$$relpath.o $$_tmpc $$_tmpl; \
			cp $$outbin $@; \
			echo "  OK: $$relpath"; \
		else \
			cat $$_tmpl >> $$compile_log; \
			rm -f $(OUT_DIRECTORY)/$$relpath.o $$_tmpc $$_tmpl; \
			echo "undefined" > $(OUT_DIRECTORY)/$$relpath.out; \
			echo "  SKIP (undefined): $$relpath"; \
			echo "  SKIP (undefined): $$relpath" >> $(SUMMARY_FILE); \
			touch $@.failed; \
		fi; \
	else \
		cat $$_tmpc > $$compile_log; \
		rm -f $(OUT_DIRECTORY)/$$relpath.o $$_tmpc $$_tmpl; \
		if grep -qE 'fatal error' $$compile_log 2>/dev/null; then \
			reason="missing_header"; \
		elif grep -qE 'undeclared|no member named' $$compile_log 2>/dev/null; then \
			reason="undeclared"; \
		elif grep -qE 'unknown type name|expected declaration' $$compile_log 2>/dev/null; then \
			reason="unknown_type"; \
		elif grep -qE 'incompatible|pointer-sign' $$compile_log 2>/dev/null; then \
			reason="incompatible"; \
		else \
			reason="compile_error"; \
		fi; \
		echo "$$reason" > $(OUT_DIRECTORY)/$$relpath.out; \
		echo "  SKIP ($$reason): $$relpath"; \
		echo "  SKIP ($$reason): $$relpath" >> $(SUMMARY_FILE); \
		touch $@.failed; \
	fi

# Special rule to handle make continuing on failed tests
.PRECIOUS: $(COMPILE_TARGETS)
%-failed:
	@# Failed tests create .failed files, continue building other tests

clean:
	rm -rf $(OUT_DIRECTORY)
	rm -rf $(OS_TEST_DEST)
