ifneq ($(KERNELRELEASE),)
obj-m := icm20608.o
else
KERNELDIR ?= /home/book/vehicle-learning/stage03/linux-imx-4.1.15-v2.8
ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
MODULE_DIR := $(CURDIR)

.PHONY: all modules clean
all: modules

modules:
	$(MAKE) -C "$(KERNELDIR)" M="$(MODULE_DIR)" ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C "$(KERNELDIR)" M="$(MODULE_DIR)" ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
endif
