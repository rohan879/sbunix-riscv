# Third-party build system for SBUnix
# Cross-compiles userspace tools for RISC-V

ROOTFS  := ../build/rootfs

.PHONY: install clean

install: busybox
	@echo "Third-party install complete"

busybox:
	@mkdir -p $(ROOTFS)/bin
	@if [ -f busybox-riscv64 ]; then \
		cp busybox-riscv64 $(ROOTFS)/bin/busybox; \
		chmod +x $(ROOTFS)/bin/busybox; \
		echo "BusyBox installed to $(ROOTFS)/bin/busybox"; \
	else \
		echo "NOTE: busybox-riscv64 not found - skipping"; \
		echo "Place a static RISC-V BusyBox binary here to enable."; \
	fi

clean:
	@echo "Third-party clean"
