/*
 * 与第 10、11 章相同的厂商协议（从 examples/11-stm32-vendor 同步）。
 * 内核模块和用户态测试程序都 include 这一份，避免 VID/PID 不一致。
 */
#ifndef MINIUSB_VENDOR_PROTOCOL_H
#define MINIUSB_VENDOR_PROTOCOL_H

#include <stdint.h>

#define VENDOR_VID 0xCAFE
#define VENDOR_PID 0x4011

#define VENDOR_EP_OUT 0x01
#define VENDOR_EP_IN  0x81
#define VENDOR_EP_SIZE 64

enum {
	VENDOR_REQ_GET_INFO = 0x01,
	VENDOR_REQ_SET_LED  = 0x02,
};

struct vendor_info {
	uint32_t uptime_ms;
	uint32_t loopback_bytes;
	uint8_t  button;
	uint8_t  led;
	uint8_t  board;
	uint8_t  reserved;
} __attribute__((packed));

#endif
