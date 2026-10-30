// 第 11 章：在真实的 STM32 上做一个厂商自定义 USB 设备。
//
//   - 批量回环：端点 0x01 收到的数据，从端点 0x81 原样发回
//   - 厂商请求：GET_INFO 读运行状态，SET_LED 开关 LED
//   - 串口日志：USB 事件打印到调试串口（115200 8N1）
//
// 主机端程序是第 10 章的 vendor_client。
#include <stdio.h>

#include "board_usb.h"
#include "tusb.h"
#include "vendor_protocol.h"

extern "C" {
#include "tick.h"
}

static uint32_t loopback_bytes;
static bool led_state;

// ---- TinyUSB 回调：设备状态变化 -------------------------------------------
void tud_mount_cb(void)
{
    printf("usb: SET_CONFIGURATION 收到，枚举完成\n");
}

void tud_umount_cb(void)
{
    printf("usb: 被主机取消配置（或拔掉了）\n");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    printf("usb: 总线空闲超过 3 ms，进入挂起\n");
}

void tud_resume_cb(void)
{
    printf("usb: 恢复\n");
}

// ---- 厂商请求 --------------------------------------------------------------
// 每个控制传输会调用这里最多三次：SETUP 阶段、DATA 阶段完成、状态阶段完成。
// 返回 false 就是 STALL —— 和第 7 章模拟器里的 on_vendor_request 一个意思。
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t* req)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_DEVICE) {
        return false;
    }
    switch (req->bRequest) {
    case VENDOR_REQ_GET_INFO: {
        static vendor_info info;   // 数据阶段结束前不能释放，所以是 static
        info.uptime_ms = millis();
        info.loopback_bytes = loopback_bytes;
        info.button = board_button();
        info.led = led_state;
        info.board = board_id();
        return tud_control_xfer(rhport, req, &info, sizeof info);
    }
    case VENDOR_REQ_SET_LED:
        led_state = req->wValue != 0;
        board_led(led_state);
        printf("vendor: SET_LED %s\n", led_state ? "on" : "off");
        return tud_control_status(rhport, req);   // 没有数据阶段，直接回状态阶段
    default:
        printf("vendor: 未知请求 0x%02X，回 STALL\n", req->bRequest);
        return false;
    }
}

// ---- 批量回环 --------------------------------------------------------------
// TinyUSB 已经替我们处理了 DATA0/DATA1、NAK、ZLP；这里只管搬数据。
// 发送缓冲满了就先不读，OUT 端点的接收缓冲随之变满，设备就会回 NAK 让主机等着。
static void loopback_task()
{
    uint32_t space = tud_vendor_write_available();
    uint32_t avail = tud_vendor_available();
    if (avail == 0 || space == 0) {
        return;
    }
    uint8_t buf[64];
    uint32_t want = avail < space ? avail : space;
    uint32_t n = tud_vendor_read(buf, want < sizeof buf ? want : sizeof buf);
    tud_vendor_write(buf, n);
    tud_vendor_write_flush();
    loopback_bytes += n;
}

int main()
{
    board_init();
    printf("\nlesson 11: vendor loopback on %s, VID:PID %04x:%04x\n", board_name(), VENDOR_VID,
           VENDOR_PID);
    if (!board_usb_start()) {
        printf("usb: 找不到 USB 控制器，协议栈没有启动\n");
        for (;;) {
        }
    }
    printf("usb: 等待主机\n");

    uint32_t last_blink = 0;
    for (;;) {
        tud_task();
        loopback_task();
        // 还没枚举成功时 LED 慢闪，方便判断卡在哪一步
        if (!tud_mounted() && millis() - last_blink > 500) {
            last_blink = millis();
            board_led(!led_state);
            led_state = !led_state;
        }
    }
}
