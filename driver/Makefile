#
# driver/Makefile — Build all kernel drivers for Ascend 310B.
#
# Prerequisites:
#   - Ascend310B-sdk/ in the project root (contains toolchain + kernel source)
#   - Or set KDIR / CROSS_COMPILE manually
#
# Usage:
#   # Default (Ascend SDK):
#   make -C driver
#
#   # Custom kernel directory:
#   make -C driver KDIR=/path/to/kernel/source
#
#   # Clean:
#   make -C driver clean
#

# ── Paths ────────────────────────────────────────────────────────────────
SDK_PATH      ?= $(CURDIR)/../Ascend310B-sdk
CROSS_COMPILE ?= $(SDK_PATH)/toolchain/bin/aarch64-linux-gnu-
KDIR          ?= $(SDK_PATH)/Ascend310B-source

ARCH          := arm64

# ── Module list ──────────────────────────────────────────────────────────
# Each subdirectory corresponds to one kernel module.
obj-m += ap3216c/
obj-m += dht11/
obj-m += gt9147/
obj-m += led_beep_jdq/
obj-m += sg90/
obj-m += sr501/

# Note: hc06_uart is a userspace program, not a kernel module.
# It should be compiled with the application CMake.

# ── Targets ───────────────────────────────────────────────────────────────
.PHONY: all clean modules modules_install

all: modules

modules:
	@echo "=========================================="
	@echo " Building drivers for Ascend 310B"
	@echo "   KDIR          = $(KDIR)"
	@echo "   CROSS_COMPILE = $(CROSS_COMPILE)"
	@echo "   ARCH           = $(ARCH)"
	@echo "=========================================="
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	rm -f Module.symvers modules.order

# ── Per-driver build targets (for debugging) ─────────────────────────────
ap3216c:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/ap3216c modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

dht11:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/dht11 modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

gt9147:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/gt9147 modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

led:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/led_beep_jdq modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

sg90:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/sg90 modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

sr501:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/sr501 modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
