/*
 * 两块板子的差异都藏在这几个函数后面：
 *   board_disco.cpp  STM32F4 Discovery（F407，OTG_FS 控制器，PA11/PA12 = CN5 micro-USB）
 *   board_dkx.cpp    立创·地阔星（F103C8，USB FS 控制器，PA11/PA12 = 板载 USB 口）
 */
#ifndef BOARD_USB_H
#define BOARD_USB_H

#include <stdint.h>

void board_init(void);          /* 时钟（含 USB 要的 48 MHz）、LED、按键、串口、SysTick */
bool board_usb_start(void);     /* USB 引脚 + 控制器时钟 + tusb_init()；找不到控制器返回 false */
void board_led(bool on);
bool board_button(void);
uint8_t board_id(void);         /* 写进 vendor_info.board */
const char* board_name(void);
const uint32_t* board_uid(void);/* 芯片 96 位唯一 ID，用来生成序列号 */

#endif
