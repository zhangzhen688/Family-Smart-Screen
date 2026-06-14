# DHT11 温湿度传感器 — OneWire GPIO 内核驱动
SDK_PATH      ?= $(CURDIR)/../../Ascend310B-sdk
CROSS_COMPILE ?= $(SDK_PATH)/toolchain/bin/aarch64-linux-gnu-
KDIR          ?= $(SDK_PATH)/Ascend310B-source
ARCH          := arm64

obj-m += dht11_drv_dtb.o

all: modules test

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

test:
	$(CROSS_COMPILE)gcc -o dht11_test test.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	rm -f dht11_test modules.order
