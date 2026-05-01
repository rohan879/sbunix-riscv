TOP     := ..
CROSS   := riscv64-unknown-elf-
CC      := $(CROSS)gcc
HOSTCC  := gcc
GCC_INC := $(shell $(CC) -print-file-name=include)
CFLAGS  := -Wall -Werror -ffreestanding -nostdlib -nostdinc -mcmodel=medany \
           -march=rv64imac_zicsr_zifencei -mabi=lp64 \
           -isystem $(abspath .)/include -isystem $(abspath $(TOP)/libc/include) -isystem $(GCC_INC) -D__sbunix__ \
           -Wno-misleading-indentation -Wno-unused-function
DUMMY_OBJ    := $(abspath $(TOP)/build/thirdparty/dummy.o)
COMPAT_OBJ   := $(abspath $(TOP)/build/thirdparty/compat.o)
SETJMP_OBJ   := $(abspath $(TOP)/build/thirdparty/setjmp.o)
SOFTFLOAT_OBJ := $(abspath $(TOP)/build/thirdparty/softfloat.o)
BB_PRINTF_OBJ := $(abspath $(TOP)/build/thirdparty/bb_printf.o)
LIBGCC       := $(shell $(CC) -print-libgcc-file-name)
LDFLAGS := -nostdlib $(abspath $(TOP)/build/libc/crt.S.o) \
           -Wl,--whole-archive $(abspath $(TOP)/build/libc.a) -Wl,--no-whole-archive \
           $(DUMMY_OBJ) $(COMPAT_OBJ) $(SETJMP_OBJ) \
           -Wl,--allow-multiple-definition -Wl,--gc-sections

$(DUMMY_OBJ): dummy.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(COMPAT_OBJ): compat.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(SETJMP_OBJ): setjmp.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(SOFTFLOAT_OBJ): softfloat.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(BB_PRINTF_OBJ): bb_printf.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

# --- Busybox ---

busybox busybox-install busybox-menuconfig: export CROSS_COMPILE=$(CROSS)
busybox busybox-install busybox-menuconfig: export EXTRA_CFLAGS=$(CFLAGS) -Wno-format
busybox busybox-install busybox-menuconfig: export EXTRA_LDFLAGS=$(LDFLAGS) $(BB_PRINTF_OBJ)

busybox-patch:
	@cd busybox && git checkout -q -- include/platform.h libbb/Kbuild.src 2>/dev/null || true
	@cd busybox && git apply ../busybox-sbunix.patch

busybox: busybox-patch $(DUMMY_OBJ) $(COMPAT_OBJ) $(SETJMP_OBJ) $(BB_PRINTF_OBJ)
	@mkdir -p $(abspath ../build/busybox)
	@test -f $(abspath ../build/busybox/.config) || cp busybox_defconfig $(abspath ../build/busybox/.config)
	@yes '' | $(MAKE) -C busybox O=$(abspath ../build/busybox) oldconfig >/dev/null 2>&1 || true
	$(MAKE) -C busybox O=$(abspath ../build/busybox) SKIP_STRIP=y

busybox-install: busybox
	$(MAKE) -C busybox O=$(abspath ../build/busybox) \
		CONFIG_PREFIX=$(abspath ../build/rootfs) install

busybox-menuconfig:
	$(MAKE) -C busybox O=$(abspath ../build/busybox) menuconfig

# --- MicroPython ---

PYTHON_DIR    := python/ports/unix
PYTHON_CFLAGS := -ffreestanding -fno-builtin -nostdlib -nostdinc -mcmodel=medany \
                 -march=rv64imac_zicsr_zifencei -mabi=lp64 \
                 -isystem $(abspath .)/include -isystem $(abspath $(TOP)/libc/include) -isystem $(GCC_INC) -D__sbunix__ \
                 -DMICROPY_NLR_SETJMP=1 -DMICROPY_NO_ALLOCA=1 -Wno-error \
                 -D_DIRENT_HAVE_D_TYPE=1
PYTHON_LDFLAGS := -nostdlib $(abspath $(TOP)/build/libc/crt.S.o) \
                  -Wl,--whole-archive $(abspath $(TOP)/build/libc.a) -Wl,--no-whole-archive \
                  $(DUMMY_OBJ) $(COMPAT_OBJ) $(SETJMP_OBJ) \
                  -Wl,--allow-multiple-definition -Wl,--no-warn-mismatch -lgcc

python-patch:
	@cd python && git checkout -q -- ports/unix/mpconfigport.h 2>/dev/null || true
	@cd python && git apply ../python-sbunix.patch

python: python-patch $(DUMMY_OBJ) $(COMPAT_OBJ) $(SETJMP_OBJ)
	$(MAKE) -C $(PYTHON_DIR) VARIANT=minimal \
		CROSS_COMPILE=$(CROSS) \
		CFLAGS_EXTRA="$(PYTHON_CFLAGS)" \
		LDFLAGS_EXTRA="$(PYTHON_LDFLAGS)" \
		LDFLAGS_ARCH="-Wl,-Map=build-minimal/micropython.map,--cref -Wl,--gc-sections"

python-install: python
	@mkdir -p $(abspath ../build/rootfs/bin) $(abspath ../build/rootfs/home)
	cp $(PYTHON_DIR)/build-minimal/micropython $(abspath ../build/rootfs/bin/python)
	cp $(PYTHON_DIR)/build-minimal/micropython $(abspath ../build/rootfs/bin/micropython)
	cp python-examples/*.py $(abspath ../build/rootfs/home/)

python-clean:
	$(MAKE) -C $(PYTHON_DIR) VARIANT=minimal clean

# --- TCC (Tiny C Compiler) ---

TCC_DIR   := tcc
TCC_BUILD := $(abspath ../build/tcc)
TCC_CFLAGS := $(CFLAGS) -Wno-implicit-function-declaration \
              -DONE_SOURCE=1 -DCONFIG_TCC_STATIC=1 -DTCC_TARGET_RISCV64 \
              -DCONFIG_TCC_PREDEFS=1 \
              -I $(TCC_DIR) -I $(TCC_DIR)/include -I $(TCC_BUILD)
TCC_LDFLAGS := $(LDFLAGS) $(SOFTFLOAT_OBJ) -Wl,--no-warn-mismatch $(LIBGCC)

$(TCC_BUILD)/tccdefs_.h: $(TCC_DIR)/include/tccdefs.h $(TCC_DIR)/conftest.c
	@mkdir -p $(TCC_BUILD)
	$(HOSTCC) -DC2STR $(TCC_DIR)/conftest.c -o $(TCC_BUILD)/c2str
	$(TCC_BUILD)/c2str $< $@
	@rm -f $(TCC_BUILD)/c2str

TCC_LIB_SRC := lib-arm64.c builtin.c dsohandle.c stdatomic.c
TCC_LIB_ASM := atomic.S
TCC_LIB_OBJ := $(addprefix $(TCC_BUILD)/lib/,$(TCC_LIB_SRC:.c=.o) $(TCC_LIB_ASM:.S=.o))
TCC_LIB_CFLAGS := -ffreestanding -nostdlib -nostdinc -mcmodel=medany \
                  -march=rv64imac_zicsr_zifencei -mabi=lp64 \
                  -isystem $(abspath .)/include -isystem $(abspath $(TOP)/libc/include) -isystem $(GCC_INC) \
                  -D__sbunix__ -Wno-unused-function

$(TCC_BUILD)/lib/%.o: $(TCC_DIR)/lib/%.c
	@mkdir -p $(@D)
	$(CC) $(TCC_LIB_CFLAGS) -c $< -o $@

$(TCC_BUILD)/lib/%.o: $(TCC_DIR)/lib/%.S
	@mkdir -p $(@D)
	$(CC) $(TCC_LIB_CFLAGS) -c $< -o $@

$(TCC_BUILD)/libtcc1.a: $(TCC_LIB_OBJ)
	$(CROSS)ar rcs $@ $^

tcc: $(DUMMY_OBJ) $(COMPAT_OBJ) $(SETJMP_OBJ) $(SOFTFLOAT_OBJ) $(TCC_BUILD)/tccdefs_.h $(TCC_BUILD)/libtcc1.a
	@mkdir -p $(TCC_BUILD)
	$(CC) $(TCC_CFLAGS) -c $(TCC_DIR)/tcc.c -o $(TCC_BUILD)/tcc.o
	$(CC) $(TCC_BUILD)/tcc.o $(TCC_LDFLAGS) -o $(TCC_BUILD)/cc

tcc-install: tcc
	@mkdir -p $(abspath ../build/rootfs/bin) $(abspath ../build/rootfs/lib/tcc) $(abspath ../build/rootfs/lib/tcc/include)
	@mkdir -p $(abspath ../build/rootfs/include/sys) $(abspath ../build/rootfs/home)
	cp $(TCC_BUILD)/cc $(abspath ../build/rootfs/bin/cc)
	cp $(TCC_BUILD)/libtcc1.a $(abspath ../build/rootfs/lib/tcc/)
	cp $(TCC_DIR)/include/*.h $(TCC_DIR)/tcclib.h $(abspath ../build/rootfs/lib/tcc/include/)
	cp $(abspath $(TOP)/libc/include)/*.h $(abspath ../build/rootfs/include/)
	cp $(abspath $(TOP)/libc/include/sys)/*.h $(abspath ../build/rootfs/include/sys/)
	cp $(abspath $(TOP)/build/libc/crt.S.o) $(abspath ../build/rootfs/lib/crt.o)
	cp $(abspath $(TOP)/build/libc.a) $(abspath ../build/rootfs/lib/libc.a)
	cp tcc-examples/* $(abspath ../build/rootfs/home/)

# --- pdpmake ---

MAKE_DIR    := make
MAKE_SRC    := $(wildcard $(MAKE_DIR)/*.c)
MAKE_OBJ    := $(addprefix $(TOP)/build/thirdparty/make/,$(notdir $(MAKE_SRC:.c=.o)))
MAKE_CFLAGS := $(CFLAGS) -Wno-unused-function -Wno-unused-variable

$(TOP)/build/thirdparty/make/%.o: $(MAKE_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(MAKE_CFLAGS) -c $< -o $@

pdpmake: $(DUMMY_OBJ) $(COMPAT_OBJ) $(MAKE_OBJ)
	$(CC) $(MAKE_OBJ) $(LDFLAGS) -o $(TOP)/build/thirdparty/make/make

pdpmake-install: pdpmake
	@mkdir -p $(abspath ../build/rootfs/bin)
	cp $(TOP)/build/thirdparty/make/make $(abspath ../build/rootfs/bin/make)

# --- Auto-discover and install all available packages ---
# Override with: THIRDPARTY_PKGS="busybox tcc" make -C thirdparty install

ifdef THIRDPARTY_PKGS
  PACKAGES := $(THIRDPARTY_PKGS)
else
  PACKAGES := $(notdir $(wildcard busybox python tcc make))
endif
INSTALL_TARGETS := $(foreach p,$(PACKAGES),$(if $(filter make,$(p)),pdpmake-install,$(p)-install))

install: $(INSTALL_TARGETS)

testsuite:
	$(MAKE) -C testsuite all
	@rm -f $(TOP)/build/tarfs.o
	$(MAKE) -C $(TOP) build/kernel.elf

test:
	python3 testsuite/run_tests.py

.PHONY: install testsuite test busybox busybox-patch busybox-install busybox-menuconfig
.PHONY: python python-patch python-install python-clean
.PHONY: tcc tcc-install
.PHONY: pdpmake pdpmake-install
