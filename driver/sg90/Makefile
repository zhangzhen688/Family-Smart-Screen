# SG90 微型舵机 — PWM 内核驱动
SDK_PATH      ?= $(CURDIR)/../../Ascend310B-sdk
CROSS_COMPILE ?= $(SDK_PATH)/toolchain/bin/aarch64-linux-gnu-
KDIR          ?= $(SDK_PATH)/Ascend310B-source
ARCH          := arm64

obj-m += sg90_drv.o

all: modules test

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

test:
	$(CROSS_COMPILE)gcc -o sg90_test sg90_test.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	rm -f sg90_test modules.order
