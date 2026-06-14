# AP3216C 环境光/接近传感器 — I2C 内核驱动
SDK_PATH      ?= $(CURDIR)/../../Ascend310B-sdk
CROSS_COMPILE ?= $(SDK_PATH)/toolchain/bin/aarch64-linux-gnu-
KDIR          ?= $(SDK_PATH)/Ascend310B-source
ARCH          := arm64

obj-m += ap3216_i2c_driver.o ap3216_i2c_client.o

all: modules test

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

test:
	$(CROSS_COMPILE)gcc -o test_ap3216 test_ap3216.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	rm -f test_ap3216 modules.order
