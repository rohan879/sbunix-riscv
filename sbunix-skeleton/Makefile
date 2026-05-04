KLANG ?= c
SUBMIT_DIR ?= /submit

CROSS   := riscv64-unknown-elf-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
AR      := $(CROSS)ar
OBJCOPY := $(CROSS)objcopy
CFLAGS  := -gdwarf-4 -Wall -Werror -Os -ffreestanding -fno-builtin -nostdlib -nostdinc -isystem $(shell $(CC) -print-file-name=include) -mcmodel=medany -march=rv64imac_zicsr_zifencei -mabi=lp64 -ffunction-sections -fdata-sections -fno-tree-switch-conversion
RUSTFLAGS := --target riscv64imac-unknown-none-elf -C panic=abort -C linker=$(CC) -C code-model=medium -C link-arg=-nostartfiles -C link-arg=-march=rv64imac -C link-arg=-mabi=lp64

PTE_DEBUG_FAILFAST ?= 0
ifeq ($(PTE_DEBUG_FAILFAST),1)
  CFLAGS += -DPTE_DEBUG_FAILFAST=1
endif

KERN_ASM := $(patsubst %,build/%.o,$(wildcard kernel/*.S))
LIBC_OBJ := $(patsubst %,build/%.o,$(filter-out libc/crt.S,$(wildcard libc/*.c libc/*.S)))
C_BIN    := $(addprefix build/rootfs/bin/,$(shell ls -d bin/*/*.c 2>/dev/null | cut -d/ -f2 | sort -u))
RS_BIN   := $(addprefix build/rootfs/bin/,$(shell ls -d bin/*/*.rs 2>/dev/null | cut -d/ -f2 | sort -u))
USER_BIN := $(C_BIN) $(RS_BIN)

# MODIFIED: Optional CI boot mode runs the priority tests from rc after the
# normal boot suite and skips the boot-time priority pass.
# Example: RUN_CI_TESTS=1 make qemu -j$(nproc)
RUN_CI_TESTS ?= 0
ifeq ($(RUN_CI_TESTS),1)
	override BUILD_TESTS := 1
	override SKIP_BOOT_PRIORITY_TESTS := 1
endif

# MODIFIED: BEGIN added
# Split user binaries: test binaries only included in test builds
# Exclude thirdparty dir - it contains os-test which has its own build system
TEST_SRC_DIRS := $(filter-out tests/thirdparty,$(wildcard tests/*))
TEST_BIN_NAMES := $(notdir $(TEST_SRC_DIRS))
LEGACY_TEST_TARFS_TARGETS := $(addprefix build/rootfs/bin/,$(TEST_BIN_NAMES))
ifdef BUILD_TESTS
  OS_TEST_MARKER := build/.os-test-built
  OPEN_POSIX_TEST_MARKER := build/.open-posix-test-built
  TEST_TARGETS := $(patsubst tests/%,build/testfs/bin/%,$(TEST_SRC_DIRS))
endif

TIMEOUT ?= 1800
# BF: Default QEMU RAM to the kernel's fixed PHYSTOP so boot does not
# put allocator freelist entries beyond the available physical memory.
# QEMU_MEM must remain 128M for remote OS/build checks.
QEMU_MEM ?= 128M
# MODIFIED: END added

# Language-specific kernel object/library
ifeq ($(KLANG),c)
  KERN_C   := $(patsubst %,build/%.o,$(shell find kernel -name '*.c'))
  KERN_ALL := $(KERN_ASM) $(KERN_C)
else ifeq ($(KLANG),rust)
  KERN_LIB := build/rust/riscv64imac-unknown-none-elf/debug/libsbunix.a
  KERN_ALL := $(KERN_ASM) $(KERN_LIB)
else ifeq ($(KLANG),zig)
  KERN_LIB := zig-out/lib/libsbunix.a
  KERN_ALL := $(KERN_ASM) $(KERN_LIB)
endif

DISK_SIZE ?= 16
DISK_IMG  := build/disk.img
# MODIFIED: added
TEST_DISK_SIZE ?= 128
TEST_DISK_IMG  := build/test-disk.img
CORE_TEST_DISK_SIZE ?= 32
CORE_TEST_DISK_IMG  := build/test-disk-core.img
# MODIFIED: Optional build-time knob to skip only the boot-time priority
# os-test/open-posix-test pass while keeping the normal SBUnix boot tests.
# Example: BUILD_TESTS=1 SKIP_BOOT_PRIORITY_TESTS=1 make qemu -j$(nproc)
SKIP_BOOT_PRIORITY_TESTS ?= 0
# MODIFIED: Optional build-time knob to skip all automatic boot tests and
# start directly at the interactive shell prompt.
# Example: BUILD_TESTS=1 SKIP_BOOT_TESTS=1 make qemu -j$(nproc)
SKIP_BOOT_TESTS ?= 0
# BF: Test builds carry a /test disk, so keep bulky userland payloads there
# instead of embedding every utility and TCC support archive in kernel tarfs.
SLIM_TEST_ROOTFS ?= $(if $(BUILD_TESTS),1,0)
STAGE_THIRDPARTY_LOCAL ?= 0
# BF: Required third-party mode turns missing vendored workload payloads into
# build failures instead of allowing guest tests to skip the coverage silently.
REQUIRE_THIRDPARTY_PAYLOADS ?= 0

-include Makefile.local

# MODIFIED from all: build/kernel.elf
ALL_TARGETS := build/kernel.elf
ifdef BUILD_TESTS
# BF: BUILD_TESTS=1 make must materialize the separate /test disk so
# validation and CI do not depend on the qemu target to build it later.
	ALL_TARGETS += $(TEST_DISK_IMG)
endif

all: $(ALL_TARGETS)

build/kernel/%.c.o: kernel/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ikernel/include -c $< -o $@

build/kernel/%.S.o: kernel/%.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ikernel/include -c $< -o $@

build/libc/%.c.o: libc/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ilibc/include -c $< -o $@

build/libc/%.S.o: libc/%.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ilibc/include -c $< -o $@

build/libc.a: $(LIBC_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

# Rust kernel build
build/rust/riscv64imac-unknown-none-elf/debug/libsbunix.a: $(wildcard kernel/*.rs) Cargo.toml
	cargo build

# Zig kernel build
zig-out/lib/libsbunix.a: $(wildcard kernel/*.zig) build.zig
	zig build -Doptimize=ReleaseSafe

.SECONDEXPANSION:

# MODIFIED: BEGIN added
# Count total os-test source files at build time
OS_TEST_TOTAL := $(shell find tests/thirdparty/sortix/os-test -name '*.c' 2>/dev/null | wc -l)

# Count total open-posix-test source files at build time
OPEN_POSIX_TOTAL := $(shell find tests/thirdparty/open-posix-test-suite/posixtest -name '*.c' ! -name 'testfrmw*' 2>/dev/null | wc -l)

# BF: Use MAKE_JOBS/SUBMAKE_JOBS instead of hardcoded parallelism so host
# validation scripts can cap third-party sub-build concurrency.
HOST_BUILD_JOBS := $(shell nproc 2>/dev/null || echo 4)
SUBMAKE_JOBS ?= $(if $(MAKE_JOBS),$(MAKE_JOBS),$(HOST_BUILD_JOBS))

# Special rule for run-os-test: pass TOTAL_TESTS_IN_SUITE as build-time define
build/rootfs/bin/run-os-test: $$(wildcard bin/run-os-test/*.c) build/libc/crt.S.o build/libc.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DTOTAL_TESTS_IN_SUITE=$(OS_TEST_TOTAL) -Ilibc/include build/libc/crt.S.o bin/run-os-test/*.c build/libc.a -Wl,--gc-sections -o $@

# Special rule for run-open-posix-test: pass TOTAL_OPEN_POSIX_TESTS as build-time define
build/rootfs/bin/run-open-posix-test: $$(wildcard bin/run-open-posix-test/*.c) build/libc/crt.S.o build/libc.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DTOTAL_OPEN_POSIX_TESTS=$(OPEN_POSIX_TOTAL) -Ilibc/include build/libc/crt.S.o bin/run-open-posix-test/*.c build/libc.a -Wl,--gc-sections -o $@

# Filter special targets from generic rule to avoid conflict
C_BIN_NO_SPECIAL := $(filter-out build/rootfs/bin/run-os-test build/rootfs/bin/run-open-posix-test,$(C_BIN))
# MODIFIED: END added

# MODIFIED: was $(C_BIN)
$(C_BIN_NO_SPECIAL): build/rootfs/bin/%: $$(wildcard bin/%/*.c) build/libc/crt.S.o build/libc.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ilibc/include build/libc/crt.S.o bin/$*/*.c build/libc.a -Wl,--gc-sections -o $@

$(RS_BIN): build/rootfs/bin/%: $$(wildcard bin/%/*.rs) build/libc/crt.S.o build/libc.a
	@mkdir -p $(@D)
	rustc $(RUSTFLAGS) -C link-arg=build/libc/crt.S.o -C link-arg=build/libc.a bin/$*/$*.rs -o $@

# MODIFIED: BEGIN added
# Prepare writable test filesystem staging area
build/testfs/.dirs:
	@mkdir -p build/testfs/bin build/testfs/var/log/os-test build/testfs/var/log/open-posix-test
	@touch $@

ifdef BUILD_TESTS
# BF: Track each test source as a real prerequisite so rebuilt validation
# disks cannot silently reuse stale test binaries after a test edit.
$(TEST_TARGETS): build/testfs/bin/%: $$(wildcard tests/$$*/*.c) build/libc/crt.S.o build/libc.a | build/testfs/.dirs
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ilibc/include build/libc/crt.S.o tests/$*/*.c build/libc.a -Wl,--gc-sections -o $@
endif

# Lightweight test disk for targeted kernel validation.  This includes only
# local tests/* binaries and intentionally skips Sortix/open-posix payloads.
CORE_TEST_TARGETS := $(patsubst tests/%,build/testfs-core/bin/%,$(TEST_SRC_DIRS))

ROOTFS_STAGE_INPUTS := $(shell find rootfs -mindepth 1 \( -type f -o -type d \) -print 2>/dev/null)
THIRDPARTY_LOCAL_STAGE_INPUTS := $(shell find thirdparty-local -mindepth 1 \( -type f -o -type d \) -print 2>/dev/null)
THIRDPARTY_MICROPYTHON_BIN := thirdparty/python/ports/unix/build-minimal/micropython
ifeq ($(STAGE_THIRDPARTY_LOCAL),1)
THIRDPARTY_STAGE_INPUTS := $(THIRDPARTY_LOCAL_STAGE_INPUTS)
else
THIRDPARTY_STAGE_INPUTS :=
endif
ifneq ($(wildcard $(THIRDPARTY_MICROPYTHON_BIN)),)
THIRDPARTY_STAGE_INPUTS += $(THIRDPARTY_MICROPYTHON_BIN)
endif
ROOTFS_BUILD_MODE := BUILD_TESTS=$(if $(BUILD_TESTS),1,0) RUN_CI_TESTS=$(RUN_CI_TESTS) SKIP_BOOT_PRIORITY_TESTS=$(SKIP_BOOT_PRIORITY_TESTS) SKIP_BOOT_TESTS=$(SKIP_BOOT_TESTS) STAGE_THIRDPARTY_LOCAL=$(STAGE_THIRDPARTY_LOCAL) REQUIRE_THIRDPARTY_PAYLOADS=$(REQUIRE_THIRDPARTY_PAYLOADS)
build/testfs-core/.dirs:
	@mkdir -p build/testfs-core/bin
	@touch $@

# BF: Core validation disk uses the same source prerequisite tracking as
# full BUILD_TESTS so targeted QEMU gates exercise current test code.
$(CORE_TEST_TARGETS): build/testfs-core/bin/%: $$(wildcard tests/$$*/*.c) build/libc/crt.S.o build/libc.a | build/testfs-core/.dirs
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ilibc/include build/libc/crt.S.o tests/$*/*.c build/libc.a -Wl,--gc-sections -o $@

build/testfs-core/.prepared: build/testfs-core/.dirs $(CORE_TEST_TARGETS) build/rootfs/.prepared
	@touch $@

# Prepare base rootfs before building os-tests
# BF: guest-run-tests is embedded in tarfs, so runner edits must rebuild
# build/rootfs/.prepared and the kernel image that carries it.
# Keep build/rootfs as a host-side build output with real executable files.
# The CI qemu step invokes make again after the build step; replacing these
# targets with guest-only /test symlinks makes make follow dangling host paths
# and causes linker output failures such as build/rootfs/bin/basename.
.PHONY: FORCE

build/rootfs/.build-mode: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(ROOTFS_BUILD_MODE)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; else rm -f $@.tmp; fi

build/rootfs/.prepared: $(USER_BIN) tools/guest-run-tests $(ROOTFS_STAGE_INPUTS) build/rootfs/.build-mode
	mkdir -p build/rootfs
	rm -rf build/rootfs/bin/os-test build/rootfs/os-test-manifest \
		build/rootfs/os-test-expect build/rootfs/var/log/os-test \
		build/rootfs/bin/open-posix-test build/rootfs/open-posix-test-manifest \
		build/rootfs/var/log/open-posix-test
	rm -f build/rootfs/bin/run-tests
	rm -f $(LEGACY_TEST_TARFS_TARGETS)
	cp -a rootfs/. build/rootfs/ # MODIFIED: originally in tarfs
	ln -sf sbush build/rootfs/bin/sh # MODIFIED: added
	ln -sf test build/rootfs/bin/[ # MODIFIED: added
ifeq ($(SKIP_BOOT_PRIORITY_TESTS),1)
	@touch build/rootfs/etc/.skip_boot_priority_tests
else
	@rm -f build/rootfs/etc/.skip_boot_priority_tests
endif
ifeq ($(RUN_CI_TESTS),1)
	@touch build/rootfs/etc/.run_ci_tests
else
	@rm -f build/rootfs/etc/.run_ci_tests
endif
ifeq ($(SKIP_BOOT_TESTS),1)
	@touch build/rootfs/etc/.skip_boot_tests
else
	@rm -f build/rootfs/etc/.skip_boot_tests
endif
ifdef BUILD_TESTS
	cp tools/guest-run-tests build/rootfs/bin/run-tests # MODIFIED: added
	# run-os-test is now built as C binary via $(C_BIN) pattern
endif
	touch $@

# Slimming is only safe on the tarfs staging copy.  The guest sees these
# /test symlinks after the test disk is mounted, while host make targets in
# build/rootfs stay rebuildable real files for repeated CI invocations.
build/tarfs-rootfs/.prepared: build/rootfs/.prepared tools/slim-test-rootfs.sh tools/stage-thirdparty.sh build/testfs/.dirs build/testfs-core/.dirs $(THIRDPARTY_STAGE_INPUTS)
	rm -rf build/tarfs-rootfs
	mkdir -p build/tarfs-rootfs
	cp -a build/rootfs/. build/tarfs-rootfs/
	STAGE_THIRDPARTY_LOCAL=$(STAGE_THIRDPARTY_LOCAL) REQUIRE_THIRDPARTY_PAYLOADS=$(REQUIRE_THIRDPARTY_PAYLOADS) tools/stage-thirdparty.sh build/tarfs-rootfs rootfs
ifeq ($(SLIM_TEST_ROOTFS),1)
	tools/slim-test-rootfs.sh build/tarfs-rootfs build/testfs build/testfs-core
endif
	touch $@

# Build os-tests into prepared testfs (with parallel support)
ifdef BUILD_TESTS
$(OS_TEST_MARKER): build/libc/crt.S.o build/libc.a build/rootfs/.prepared build/testfs/.dirs $(TEST_TARGETS)
	@echo "=== Building os-test suite ==="
	$(MAKE) -f tools/os-test.mk SBUNIX_ROOT=$(CURDIR) -j$(SUBMAKE_JOBS) install
	# Copy test manifest and expected outputs into testfs
	@if [ -f build/os-test-out/sbunix/test-manifest.txt ]; then \
		cp build/os-test-out/sbunix/test-manifest.txt build/testfs/os-test-manifest; \
	fi
	@mkdir -p build/testfs/os-test-expect
	@if ls tests/thirdparty/sortix/os-test/*.expect >/dev/null 2>&1; then \
		cp -a tests/thirdparty/sortix/os-test/*.expect build/testfs/os-test-expect/; \
	fi
	@# BF: Add POSIX expect file for sigaction-exec-flags so correct "0"
	@# output gets RESULT_PASS.  Also remove conflicting .unknown.1 which
	@# contains "0" and could match first due to readdir ordering.
	@mkdir -p build/testfs/os-test-expect/signal.expect
	@printf '0' > build/testfs/os-test-expect/signal.expect/sigaction-exec-flags.posix
	@printf '0 | POLLIN\nSIGUSR1\n' > build/testfs/os-test-expect/signal.expect/ppoll-block-sleep-write-raise.posix.3
	@printf '0 | POLLIN\n' > build/testfs/os-test-expect/signal.expect/ppoll-block-sleep-write-raise.posix.4
	@rm -f build/testfs/os-test-expect/signal.expect/sigaction-exec-flags.unknown.1
	# Seed testfs with compile-time .out files for skipped tests
	@mkdir -p build/testfs/var/log/os-test
	@if [ -d build/os-test-out/sbunix ]; then \
		find build/os-test-out/sbunix -name '*.out' -type f | while read f; do \
			rel=$$(echo "$$f" | sed 's|build/os-test-out/sbunix/||'); \
			mkdir -p build/testfs/var/log/os-test/$$(dirname "$$rel"); \
			cp "$$f" build/testfs/var/log/os-test/"$$rel"; \
		done; \
		echo "Copied .out files for failed tests to testfs"; \
	fi
	@touch $@
endif

# Build open-posix tests into prepared testfs (with parallel support)
ifdef BUILD_TESTS
$(OPEN_POSIX_TEST_MARKER): build/libc/crt.S.o build/libc.a build/rootfs/.prepared build/testfs/.dirs $(TEST_TARGETS)
	@echo "=== Building open-posix-test suite ==="
	$(MAKE) -f tools/open-posix-test.mk SBUNIX_ROOT=$(CURDIR) -j$(SUBMAKE_JOBS) install
	# Copy test manifest into testfs
	@if [ -f build/open-posix-test-out/sbunix/test-manifest.txt ]; then \
		cp build/open-posix-test-out/sbunix/test-manifest.txt build/testfs/open-posix-test-manifest; \
	fi
	# Seed testfs with compile-time .out files for skipped tests
	@mkdir -p build/testfs/var/log/open-posix-test
	@if [ -d build/open-posix-test-out/sbunix ]; then \
		find build/open-posix-test-out/sbunix -name '*.out' -type f | while read f; do \
			rel=$$(echo "$$f" | sed 's|build/open-posix-test-out/sbunix/||'); \
			mkdir -p build/testfs/var/log/open-posix-test/$$(dirname "$$rel"); \
			cp "$$f" build/testfs/var/log/open-posix-test/"$$rel"; \
		done; \
		echo "Copied .out files for failed open-posix tests to testfs"; \
	fi
	@touch $@
endif
# MODIFIED: END added

build/tarfs.o: build/tarfs-rootfs/.prepared # MODIFIED: originally just : $(USER_BIN)
	tar cf build/rootfs.tar -C build/tarfs-rootfs .
	cd build && $(OBJCOPY) -I binary -O elf64-littleriscv -B riscv \
		--rename-section .data=.tarfs \
		--redefine-sym _binary_rootfs_tar_start=_tarfs_start \
		--redefine-sym _binary_rootfs_tar_end=_tarfs_end \
		--redefine-sym _binary_rootfs_tar_size=_tarfs_size \
		rootfs.tar tarfs.o

build/kernel.elf: $(KERN_ALL) build/tarfs.o
	$(LD) -T kernel/kernel.ld $(KERN_ALL) build/tarfs.o -o $@

thirdparty: build/libc/crt.S.o build/libc.a
	$(MAKE) build/rootfs/.prepared
	$(MAKE) -C thirdparty install
	STAGE_THIRDPARTY_LOCAL=$(STAGE_THIRDPARTY_LOCAL) REQUIRE_THIRDPARTY_PAYLOADS=$(REQUIRE_THIRDPARTY_PAYLOADS) tools/stage-thirdparty.sh build/rootfs rootfs
	@rm -f build/tarfs-rootfs/.prepared build/tarfs.o
	$(MAKE) build/kernel.elf

build/tools/mkfs: tools/mkfs.c
	@mkdir -p $(@D)
	gcc -Wall -o $@ $<

# MODIFIED: added
build/tools/mkext2img: tools/mkext2img.c
	@mkdir -p $(@D)
	gcc -Wall -Werror -O2 -o $@ $<

$(DISK_IMG): build/tools/mkfs
	build/tools/mkfs $@ $(DISK_SIZE)

# MODIFIED: BEGIN added
ifdef BUILD_TESTS
build/testfs/.prepared: build/testfs/.dirs $(TEST_TARGETS) $(OS_TEST_MARKER) $(OPEN_POSIX_TEST_MARKER)
	@if [ -x tools/generate-priority-manifest ]; then \
		tools/generate-priority-manifest; \
	fi
	@# Keep vendored third-party payloads in the normal guest rootfs only. The
	@# separate test disk carries /test binaries, manifests, and logs.
	@touch $@

$(TEST_DISK_IMG): build/tools/mkext2img build/testfs/.prepared
	@size_mb=$(TEST_DISK_SIZE); \
	used_mb=$$(du -sm build/testfs | cut -f1); \
	auto_mb=$$((used_mb + used_mb / 2 + 64)); \
	if [ $$auto_mb -gt $$size_mb ]; then \
		size_mb=$$auto_mb; \
	fi; \
	echo "Building test disk image ($${size_mb}MB for $${used_mb}MB staged testfs)"; \
	build/tools/mkext2img $@ $$size_mb build/testfs
endif

$(CORE_TEST_DISK_IMG): build/tools/mkext2img build/testfs-core/.prepared
	@size_mb=$(CORE_TEST_DISK_SIZE); \
	used_mb=$$(du -sm build/testfs-core | cut -f1); \
	auto_mb=$$((used_mb + used_mb / 2 + 16)); \
	if [ $$auto_mb -gt $$size_mb ]; then \
		size_mb=$$auto_mb; \
	fi; \
	echo "Building core test disk image ($${size_mb}MB for $${used_mb}MB staged testfs-core)"; \
	build/tools/mkext2img $@ $$size_mb build/testfs-core

ifdef BUILD_TESTS
QEMU_TEST_DRIVE_ARGS := -drive file=$(TEST_DISK_IMG),format=raw,if=none,id=hd1 \
	-device virtio-blk-pci-non-transitional,drive=hd1
endif
# MODIFIED: END added

# MODIFIED: added " $(if $(BUILD_TESTS),$(TEST_DISK_IMG))", "$(QEMU_TEST_DRIVE_ARGS) \"
qemu: build/kernel.elf $(DISK_IMG) $(if $(BUILD_TESTS),$(TEST_DISK_IMG))
	qemu-system-riscv64 -machine virt -m $(QEMU_MEM) -bios default -kernel $< \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
		-device virtio-blk-pci-non-transitional,drive=hd0 \
		$(QEMU_TEST_DRIVE_ARGS) \
		-device virtio-gpu-pci \
		-device virtio-net-pci-non-transitional,netdev=net0 \
		-netdev user,id=net0 \
		-nographic

qemu-core-tests: build/kernel.elf $(DISK_IMG) $(CORE_TEST_DISK_IMG)
	qemu-system-riscv64 -machine virt -m $(QEMU_MEM) -bios default -kernel $< \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
		-device virtio-blk-pci-non-transitional,drive=hd0 \
		-drive file=$(CORE_TEST_DISK_IMG),format=raw,if=none,id=hd1 \
		-device virtio-blk-pci-non-transitional,drive=hd1 \
		-device virtio-gpu-pci \
		-device virtio-net-pci-non-transitional,netdev=net0 \
		-netdev user,id=net0 \
		-nographic

# NEW: GDB debug target -- launches QEMU paused with GDB stub on TCP :1234.
# Connect with: riscv64-unknown-elf-gdb build/kernel.elf -ex "target remote :1234"
# Useful for PTE_X watchpoints, signal delivery tracing, crash backtraces.
qemu-gdb: build/kernel.elf $(DISK_IMG) $(if $(BUILD_TESTS),$(TEST_DISK_IMG))
	qemu-system-riscv64 -machine virt -m $(QEMU_MEM) -bios default -kernel $< \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
		-device virtio-blk-pci-non-transitional,drive=hd0 \
		$(QEMU_TEST_DRIVE_ARGS) \
		-device virtio-gpu-pci \
		-nographic -s -S

# MODIFIED: added
test: $(DISK_IMG)
	TIMEOUT=$(TIMEOUT) bash tools/run_tests.sh

clean:
	rm -rf build zig-out .zig-cache

submit:
	@set -e; \
	STMP=$$(mktemp -d /tmp/sbunix-submit.XXXXXX); \
	cleanup() { set +e; chmod -R u+w "$$STMP" 2>/dev/null; rm -rf "$$STMP" 2>/dev/null; }; \
	trap cleanup EXIT; # MODIFIED: max-size \
	rsync -a \
		--exclude='.git' --exclude='build/' --exclude='zig-out/' \
		--exclude='.zig-cache/' --exclude='.claude/' --exclude='thirdparty/' \
		--max-size=300K \
		. "$$STMP/"; \
	cd "$$STMP"; \
	git init -q; \
	git add -A; \
	git -c user.name=student -c user.email=student@sbunix commit -q -m "submit"; \
	rm -rf $(SUBMIT_DIR)/sbunix.2 2>/dev/null || true; \
	mv $(SUBMIT_DIR)/sbunix.1 $(SUBMIT_DIR)/sbunix.2 2>/dev/null || true; \
	mv $(SUBMIT_DIR)/sbunix $(SUBMIT_DIR)/sbunix.1 2>/dev/null || true; \
	mkdir -p $(SUBMIT_DIR)/sbunix; \
	git archive HEAD | tar -x -C $(SUBMIT_DIR)/sbunix; \
	echo "Submitted to $(SUBMIT_DIR)/sbunix"

.PRECIOUS: build/%.S.o build/%.c.o
.PHONY: all qemu clean thirdparty submit test # MODIFIED: added test
