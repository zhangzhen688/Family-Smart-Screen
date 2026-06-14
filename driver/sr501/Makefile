# SR501 PIR 人体红外传感器 — GPIO 中断内核驱动
SDK_PATH      ?= $(CURDIR)/../../Ascend310B-sdk
CROSS_COMPILE ?= $(SDK_PATH)/toolchain/bin/aarch64-linux-gnu-
KDIR          ?= $(SDK_PATH)/Ascend310B-source
ARCH          := arm64

obj-m += sr501_drv_dtb.o

all: modules test

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

test:
	$(CROSS_COMPILE)gcc -o sr501_test test.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	rm -f sr501_test modules.order
