# 第 13 章 Linux 内核 USB 驱动示例
#
#   make step1 && sudo insmod miniusb_step1.ko
#   make step3 && sudo insmod miniusb_vendor.ko && make test
#
# 需要 linux-headers 与 uname -r 一致（WSL2 用微软内核时需对应 headers 包）。

ifneq ($(KERNELRELEASE),)
obj-m := $(MINIUSB_MODULE).o
else

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
export MINIUSB_MODULE

.PHONY: all step1 step2 step3 clean test help

all: step3

help:
	@echo "目标: step1 | step2 | step3 | test | clean"
	@echo "KERNEL_DIR=$(KERNEL_DIR)"

step1:
	@$(MAKE) MINIUSB_MODULE=miniusb_step1 kbuild

step2:
	@$(MAKE) MINIUSB_MODULE=miniusb_step2 kbuild

step3:
	@$(MAKE) MINIUSB_MODULE=miniusb_vendor kbuild

kbuild:
	@if [ ! -f "$(KERNEL_DIR)/Makefile" ]; then \
		echo "未找到 $(KERNEL_DIR)"; \
		echo "安装：sudo apt install linux-headers-$$(uname -r)"; \
		exit 1; \
	fi
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) MINIUSB_MODULE=$(MINIUSB_MODULE) modules

clean:
	@if [ -f "$(KERNEL_DIR)/Makefile" ]; then \
		$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean 2>/dev/null || true; \
	fi
	rm -f test_chardev

test: test_chardev.c vendor_protocol.h
	gcc -Wall -Wextra -o test_chardev test_chardev.c

endif
