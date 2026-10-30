# 第 11 章固件：复用 ../../../stm32 教程的构建框架、启动代码和 TinyUSB。
#
#   make                 STM32F4 Discovery（默认），产物在 build/
#   make BOARD=dkx       立创·地阔星 STM32F103C8T6，产物在 build-dkx/
#
# 如果 stm32 教程不在旁边，用 STM32_EXAMPLES=/path/to/stm32/examples 指定。
STM32_EXAMPLES ?= ../../../stm32/examples
BOARD ?= disco

TUSB := $(STM32_EXAMPLES)/third_party/tinyusb/src

SRCS := main.cpp usb_descriptors.cpp \
        $(TUSB)/tusb.c \
        $(TUSB)/common/tusb_fifo.c \
        $(TUSB)/device/usbd.c \
        $(TUSB)/class/vendor/vendor_device.c

ifeq ($(BOARD),dkx)
SRCS += board_dkx.cpp \
        $(TUSB)/portable/st/stm32_fsdev/dcd_stm32_fsdev.c \
        $(TUSB)/portable/st/stm32_fsdev/fsdev_common.c
# fsdev_common.c 用了 GNU 扩展的 asm 关键字，而框架用严格的 -std=c11 编译
CFLAGS += -Dasm=__asm__
else
SRCS += board_disco.cpp \
        $(TUSB)/portable/synopsys/dwc2/dcd_dwc2.c \
        $(TUSB)/portable/synopsys/dwc2/dwc2_common.c
endif

CPPFLAGS += -I$(TUSB)

include $(STM32_EXAMPLES)/common/cmsis.mk
