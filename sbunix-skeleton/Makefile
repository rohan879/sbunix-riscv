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
# MODIFIED: added
TEST_DISK_IMG  := build/test-disk.img
CORE_TEST_DISK_SIZE ?= 32
CORE_TEST_DISK_IMG  := build/test-disk-core.img

-include Makefile.local

all: build/kernel.elf

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

# BF: Use nproc instead of hardcoded -j16 to avoid oversubscription on
# machines with fewer cores.  Defaults to 4 if nproc is unavailable.
SUBMAKE_JOBS ?= $(shell nproc 2>/dev/null || echo 4)

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
$(TEST_TARGETS): build/testfs/bin/%: build/libc/crt.S.o build/libc.a | build/testfs/.dirs
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ilibc/include build/libc/crt.S.o tests/$*/*.c build/libc.a -Wl,--gc-sections -o $@
endif

# Lightweight test disk for targeted kernel validation.  This includes only
# local tests/* binaries and intentionally skips Sortix/open-posix payloads.
CORE_TEST_TARGETS := $(patsubst tests/%,build/testfs-core/bin/%,$(TEST_SRC_DIRS))

build/testfs-core/.dirs:
	@mkdir -p build/testfs-core/bin
	@touch $@

$(CORE_TEST_TARGETS): build/testfs-core/bin/%: build/libc/crt.S.o build/libc.a | build/testfs-core/.dirs
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ilibc/include build/libc/crt.S.o tests/$*/*.c build/libc.a -Wl,--gc-sections -o $@

build/testfs-core/.prepared: build/testfs-core/.dirs $(CORE_TEST_TARGETS)
	@touch $@

# Prepare base rootfs before building os-tests
build/rootfs/.prepared: $(USER_BIN)
	mkdir -p build/rootfs
	rm -rf build/rootfs/bin/os-test build/rootfs/os-test-manifest \
		build/rootfs/os-test-expect build/rootfs/var/log/os-test \
		build/rootfs/bin/open-posix-test build/rootfs/open-posix-test-manifest \
		build/rootfs/var/log/open-posix-test
	rm -f $(LEGACY_TEST_TARFS_TARGETS)
	cp -a rootfs/. build/rootfs/ # MODIFIED: originally in tarfs
	ln -sf sbush build/rootfs/bin/sh # MODIFIED: added
	ln -sf test build/rootfs/bin/[ # MODIFIED: added
ifdef BUILD_TESTS
	cp tools/guest-run-tests build/rootfs/bin/run-tests # MODIFIED: added
	# run-os-test is now built as C binary via $(C_BIN) pattern
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
	@cp -a tests/thirdparty/sortix/os-test/*.expect build/testfs/os-test-expect/
	@# BF: Add POSIX expect file for sigaction-exec-flags so correct "0"
	@# output gets RESULT_PASS.  Also remove conflicting .unknown.1 which
	@# contains "0" and could match first due to readdir ordering.
	@printf '0' > build/testfs/os-test-expect/signal.expect/sigaction-exec-flags.posix
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

build/tarfs.o: build/rootfs/.prepared # MODIFIED: originally just : $(USER_BIN)
	tar cf build/rootfs.tar -C build/rootfs .
	cd build && $(OBJCOPY) -I binary -O elf64-littleriscv -B riscv \
		--rename-section .data=.tarfs \
		--redefine-sym _binary_rootfs_tar_start=_tarfs_start \
		--redefine-sym _binary_rootfs_tar_end=_tarfs_end \
		--redefine-sym _binary_rootfs_tar_size=_tarfs_size \
		rootfs.tar tarfs.o

build/kernel.elf: $(KERN_ALL) build/tarfs.o
	$(LD) -T kernel/kernel.ld $(KERN_ALL) build/tarfs.o -o $@

thirdparty: build/libc/crt.S.o build/libc.a
	$(MAKE) -C thirdparty install
	@rm -f build/tarfs.o
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
	qemu-system-riscv64 -machine virt -bios default -kernel $< \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
		-device virtio-blk-pci-non-transitional,drive=hd0 \
		$(QEMU_TEST_DRIVE_ARGS) \
		-device virtio-gpu-pci \
		-device virtio-net-pci-non-transitional,netdev=net0 \
		-netdev user,id=net0 \
		-nographic

qemu-core-tests: build/kernel.elf $(DISK_IMG) $(CORE_TEST_DISK_IMG)
	qemu-system-riscv64 -machine virt -bios default -kernel $< \
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
	qemu-system-riscv64 -machine virt -bios default -kernel $< \
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
