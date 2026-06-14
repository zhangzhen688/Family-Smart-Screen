# LED/蜂鸣器/继电器 — 通用 GPIO 输出内核驱动
SDK_PATH      ?= $(CURDIR)/../../Ascend310B-sdk
CROSS_COMPILE ?= $(SDK_PATH)/toolchain/bin/aarch64-linux-gnu-
KDIR          ?= $(SDK_PATH)/Ascend310B-source
ARCH          := arm64

obj-m += led_drv.o

all: modules test

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

test:
	$(CROSS_COMPILE)gcc -o led_test led_test.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	rm -f led_test modules.order
