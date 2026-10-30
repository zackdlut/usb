/*
 * 第 11 章固件和第 10 章 vendor_client 共用的“协议”：
 * 一个厂商自定义类（0xFF）的设备，主机和设备事先约定好下面这些。
 *
 *   端点 0x01 批量 OUT ┐ 回环：OUT 端点收到什么，就从 IN 端点原样发回去
 *   端点 0x81 批量 IN  ┘
 *
 *   厂商请求（控制传输，bmRequestType 的类型位 = 厂商，接收者 = 设备）
 *     GET_INFO  IN   wLength = sizeof(vendor_info)  读设备状态
 *     SET_LED   OUT  wValue  = 0 灭 / 1 亮          无数据阶段
 */
#ifndef VENDOR_PROTOCOL_H
#define VENDOR_PROTOCOL_H

#include <stdint.h>

#define VENDOR_VID 0xCAFE   /* TinyUSB 的测试 VID：自己桌上用没问题，产品必须申请 */
#define VENDOR_PID 0x4011

#define VENDOR_EP_OUT 0x01
#define VENDOR_EP_IN  0x81
#define VENDOR_EP_SIZE 64

enum {
    VENDOR_REQ_GET_INFO = 0x01,
    VENDOR_REQ_SET_LED  = 0x02,
};

/* 小端，和 ARM、x86 一致，所以两边直接用同一个结构体 */
struct vendor_info {
    uint32_t uptime_ms;        /* 上电以来的毫秒数 */
    uint32_t loopback_bytes;   /* 回环过的总字节数 */
    uint8_t  button;           /* 用户按键：1 = 按下 */
    uint8_t  led;              /* LED：1 = 亮 */
    uint8_t  board;            /* 1 = STM32F4 Discovery，2 = 地阔星 F103 */
    uint8_t  reserved;
} __attribute__((packed));

#ifdef __cplusplus
static_assert(sizeof(vendor_info) == 12, "vendor_info must stay 12 bytes");
#endif

#endif
