// 描述符：和第 6 章用 ConfigBuilder 拼出来的是同一种东西，
// 只不过这里用 TinyUSB 的宏，在编译期就排成常量数组放进 Flash。
// 主机发 GET_DESCRIPTOR 时，TinyUSB 回调下面这几个函数取数据。
#include <string.h>

#include "board_usb.h"
#include "tusb.h"
#include "vendor_protocol.h"

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,          // 类别由接口描述符说明
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = VENDOR_VID,
    .idProduct = VENDOR_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t* tud_descriptor_device_cb(void)
{
    return reinterpret_cast<const uint8_t*>(&desc_device);
}

// 配置描述符 9 字节 + 接口 9 字节 + 两个端点各 7 字节 = 32 字节
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const uint8_t desc_configuration[] = {
    // 配置值 1，1 个接口，无字符串，总长，属性（总线供电），最大 100 mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    // 接口 0，字符串 4，OUT 端点，IN 端点，包长
    TUD_VENDOR_DESCRIPTOR(0, 4, VENDOR_EP_OUT, VENDOR_EP_IN, VENDOR_EP_SIZE),
};
static_assert(sizeof desc_configuration == CONFIG_TOTAL_LEN, "wTotalLength mismatch");

const uint8_t* tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

// 字符串描述符：UTF-16LE，第一个 16 位字是 (类型 << 8) | 总字节数
const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[1 + 32];
    static const char* const strings[] = {
        nullptr,                     // 0：语言列表，下面单独处理
        "miniusb tutorial",          // 1：厂商
        "miniusb vendor loopback",   // 2：产品
        nullptr,                     // 3：序列号，用芯片唯一 ID 生成
        "loopback interface",        // 4：接口
    };
    size_t n = 0;
    if (index == 0) {
        buf[1] = 0x0409;
        n = 1;
    } else if (index == 3) {
        // 96 位唯一 ID 的前 64 位写成 16 个十六进制字符：同一台电脑插多块板子也能区分
        static const char hexdigits[] = "0123456789ABCDEF";
        const uint32_t* uid = board_uid();
        for (int w = 0; w < 2; w++) {
            for (int nib = 7; nib >= 0; nib--) {
                buf[1 + n++] = static_cast<uint16_t>(hexdigits[(uid[w] >> (nib * 4)) & 0xF]);
            }
        }
    } else if (index < sizeof strings / sizeof strings[0]) {
        const char* s = strings[index];
        for (; s[n] && n < 32; n++) {
            buf[1 + n] = static_cast<uint8_t>(s[n]);
        }
    } else {
        return nullptr;   // 没有这个字符串：TinyUSB 回 STALL
    }
    buf[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return buf;
}
