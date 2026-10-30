/* TinyUSB 配置：一个厂商自定义类设备（批量回环 + 厂商控制请求）。 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

/* lesson.mk 按 BOARD 定义了芯片宏，这里据此选择 USB 控制器驱动 */
#if defined(STM32F407xx)
#define CFG_TUSB_MCU            OPT_MCU_STM32F4     /* OTG_FS，Synopsys DWC2 核 */
#elif defined(STM32F103xB)
#define CFG_TUSB_MCU            OPT_MCU_STM32F1     /* USB FS device，ST 自己的 FSDEV 核 */
#else
#error "unknown board"
#endif

#define CFG_TUSB_OS             OPT_OS_NONE         /* tud_task() 在主循环里调用 */
#define CFG_TUSB_DEBUG          0

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          1

#define CFG_TUD_VENDOR_EPSIZE     64
#define CFG_TUD_VENDOR_RX_BUFSIZE 512
#define CFG_TUD_VENDOR_TX_BUFSIZE 512

#endif
