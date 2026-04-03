# tools/open-posix-test.mk - Cross-compile Open POSIX Test Suite for SBUnix/RISC-V
#
# This Makefile cross-compiles the Open POSIX Test Suite for SBUnix.
# Tests that fail to compile are logged but skipped (no stubs created).
# This shows which POSIX features SBUnix is missing.
#
# Usage from sbunix-skeleton/:
#   make -f tools/open-posix-test.mk SBUNIX_ROOT=$(pwd) install
#
# Or via the main Makefile with BUILD_TESTS=1

SBUNIX_ROOT ?= $(CURDIR)/..
POSIXTEST_SRC := $(SBUNIX_ROOT)/tests/thirdparty/open-posix-test-suite/posixtest
CROSS := riscv64-unknown-elf-

CC := $(CROSS)gcc
OS := SBUnix
OUT_OS := sbunix
OUT_DIRECTORY := $(SBUNIX_ROOT)/build/open-posix-test-out/$(OUT_OS)

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
CPPFLAGS := -D_POSIX_C_SOURCE=200112L -D_GNU_SOURCE -D_BSD_SOURCE \
            -D_ALL_SOURCE -D_DEFAULT_SOURCE

# We can't use -Werror since many tests intentionally test for missing features
CFLAGS_WARN := -Wall -Wextra -Werror=implicit-function-declaration

# Linker flags - CRT must be first, libc last
LDFLAGS := $(CRT_OBJ) $(LIBC_A) -Wl,--gc-sections -nostdlib

# Install target: copy compiled binaries to build/testfs
OPEN_POSIX_TEST_DEST ?= $(SBUNIX_ROOT)/build/testfs/bin/open-posix-test

# Summary and log files for build results
SUMMARY_FILE := $(OUT_DIRECTORY)/build-summary.txt
COMPILE_LOG_DIR := $(OUT_DIRECTORY)/compile-logs
MANIFEST_FILE := $(OUT_DIRECTORY)/test-manifest.txt

# Suppress log files for tests that compile and link without warnings
OPEN_POSIX_SUPPRESS_OK_LOGS ?= 1

# Enable parallel builds
MAKEFLAGS += --output-sync=recurse

# Find all test source files (conformance, functional, stress)
# Skip testfrmw.c (included by test files, not compiled standalone)
ALL_SOURCES := $(shell find $(POSIXTEST_SRC) -name '*.c' \
               ! -name 'testfrmw*' 2>/dev/null | sort)
COMPILE_TARGETS := $(patsubst $(POSIXTEST_SRC)/%.c,$(OPEN_POSIX_TEST_DEST)/%,$(ALL_SOURCES))

.PHONY: all install clean setup

all: setup $(COMPILE_TARGETS)
	@echo ""
	@echo "=== open-posix-test build complete ==="
	@# Generate manifest from actually compiled binaries
	@echo "# Test manifest for SBUnix open-posix-test" > $(MANIFEST_FILE)
	@echo "# Format: /test/bin/open-posix-test/path/to/test" >> $(MANIFEST_FILE)
	@find $(OPEN_POSIX_TEST_DEST) -type f ! -name '*.failed' -executable | sort | \
		sed 's|$(SBUNIX_ROOT)/build/testfs|/test|' >> $(MANIFEST_FILE)
	@# Generate summary of failed test outcomes (.out files)
	@find $(OUT_DIRECTORY) -name '*.out' -type f | sort | while read f; do \
		rel=$$(echo "$$f" | sed 's|$(OUT_DIRECTORY)/||;s|\.out$$||'); \
		outcome=$$(cat "$$f"); \
		echo "$$rel: $$outcome" >> $(SUMMARY_FILE); \
	done
	@ok_count=$$(find $(OPEN_POSIX_TEST_DEST) -type f ! -name '*.failed' | wc -l); \
	total_count=$$(find $(POSIXTEST_SRC) -name '*.c' ! -name 'testfrmw*' | wc -l); \
	skip_count=$$((total_count - ok_count)); \
	out_count=$$(find $(OUT_DIRECTORY) -name '*.out' | wc -l); \
	echo "Compiled: $$ok_count tests"; \
	echo "Skipped:  $$skip_count tests (missing features)"; \
	echo ".out files generated: $$out_count"; \
	echo ""; \
	echo "Binaries installed to: $(OPEN_POSIX_TEST_DEST)"; \
	echo "Compilation logs: $(COMPILE_LOG_DIR)/<category>/<test>.log"; \
	echo "Test manifest: $(MANIFEST_FILE) ($$ok_count entries)"

setup:
	@echo "=== Building open-posix-test for SBUnix (RISC-V) ==="
	@echo "CC=$(CC)"
	@echo "SBUNIX_ROOT=$(SBUNIX_ROOT)"
	@echo "POSIXTEST_SRC=$(POSIXTEST_SRC)"
	@echo ""
	@mkdir -p $(OUT_DIRECTORY)
	@rm -rf $(COMPILE_LOG_DIR)
	@mkdir -p $(COMPILE_LOG_DIR)
	@mkdir -p $(OPEN_POSIX_TEST_DEST)
	@echo "open-posix-test build summary for SBUnix" > $(SUMMARY_FILE)
	@echo "=========================================" >> $(SUMMARY_FILE)
	@echo "" >> $(SUMMARY_FILE)

install: all

# Pattern rule for compiling individual tests
# Each conformance test gets -I<its own directory> so it can find testfrmw.c/.h
$(OPEN_POSIX_TEST_DEST)/%: $(POSIXTEST_SRC)/%.c | setup
	@relpath=$*; \
	dir=$$(dirname $$relpath); \
	name=$$(basename $$relpath); \
	mkdir -p $(OUT_DIRECTORY)/$$dir; \
	mkdir -p $(OPEN_POSIX_TEST_DEST)/$$dir; \
	outbin=$(OUT_DIRECTORY)/$$relpath; \
	compile_log=$(COMPILE_LOG_DIR)/$$relpath.log; \
	test_src_dir=$$(dirname $<); \
	rm -f $$outbin $(OUT_DIRECTORY)/$$relpath.o; \
	_tmpc=$$(mktemp); \
	_tmpl=$$(mktemp); \
	if $(CC) $(CFLAGS) $(CFLAGS_WARN) $(CPPFLAGS) -I$$test_src_dir -I$(POSIXTEST_SRC)/include -c $< -o $(OUT_DIRECTORY)/$$relpath.o 2>$$_tmpc; then \
		if $(CC) $(LDFLAGS) $(OUT_DIRECTORY)/$$relpath.o -o $$outbin 2>$$_tmpl; then \
			if [ -s $$_tmpc ] || [ -s $$_tmpl ]; then \
				mkdir -p $$(dirname $$compile_log); \
				cat $$_tmpc > $$compile_log; \
				cat $$_tmpl >> $$compile_log; \
			fi; \
			rm -f $(OUT_DIRECTORY)/$$relpath.o $$_tmpc $$_tmpl; \
			cp $$outbin $@; \
			echo "  OK: $$relpath"; \
		else \
			mkdir -p $$(dirname $$compile_log); \
			cat $$_tmpc > $$compile_log; \
			cat $$_tmpl >> $$compile_log; \
			rm -f $(OUT_DIRECTORY)/$$relpath.o $$_tmpc $$_tmpl; \
			echo "undefined" > $(OUT_DIRECTORY)/$$relpath.out; \
			echo "  SKIP (undefined): $$relpath"; \
			echo "  SKIP (undefined): $$relpath" >> $(SUMMARY_FILE); \
			touch $@.failed; \
		fi; \
	else \
		mkdir -p $$(dirname $$compile_log); \
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
	rm -rf $(OPEN_POSIX_TEST_DEST)
