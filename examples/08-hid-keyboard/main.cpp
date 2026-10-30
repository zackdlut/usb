// 第 8 章：HID 类 —— 做一个 USB 键盘。
//
// 设备端：HID 描述符 + 报告描述符 + 一个中断 IN 端点，固件“敲”出一句话。
// 主机端：一个迷你 HID 驱动 —— 读报告描述符并解析，每 bInterval 毫秒轮询
//         一次中断端点，把报告翻译回字符；最后用 SET_REPORT 点亮 Caps Lock 灯。
#include <cstdio>
#include <deque>
#include <string>

#include "hid_report.hpp"
#include "miniusb/device.hpp"
#include "miniusb/host.hpp"

using namespace musb;

static void title(const char* t)
{
    std::printf("\n========== %s ==========\n", t);
}

// HID 1.11 规范附录 B.1 的“启动键盘”报告描述符，BIOS 都认识它。
// 输入报告 8 字节：[修饰键位图][保留][键码1..键码6]
// 输出报告 1 字节：bit0 NumLock  bit1 CapsLock  bit2 ScrollLock ...
static const Bytes kKeyboardReport = {
    0x05, 0x01,   // Usage Page (Generic Desktop)
    0x09, 0x06,   // Usage (Keyboard)
    0xA1, 0x01,   // Collection (Application)
    0x05, 0x07,   //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,   //   Usage Minimum (224)  左 Ctrl
    0x29, 0xE7,   //   Usage Maximum (231)  右 GUI
    0x15, 0x00,   //   Logical Minimum (0)
    0x25, 0x01,   //   Logical Maximum (1)
    0x75, 0x01,   //   Report Size (1)
    0x95, 0x08,   //   Report Count (8)
    0x81, 0x02,   //   Input (Data,Var,Abs)   第 0 字节：8 个修饰键各占 1 位
    0x95, 0x01,   //   Report Count (1)
    0x75, 0x08,   //   Report Size (8)
    0x81, 0x01,   //   Input (Const)          第 1 字节：保留
    0x95, 0x05,   //   Report Count (5)
    0x75, 0x01,   //   Report Size (1)
    0x05, 0x08,   //   Usage Page (LEDs)
    0x19, 0x01,   //   Usage Minimum (1)
    0x29, 0x05,   //   Usage Maximum (5)
    0x91, 0x02,   //   Output (Data,Var,Abs)  5 个 LED
    0x95, 0x01,   //   Report Count (1)
    0x75, 0x03,   //   Report Size (3)
    0x91, 0x01,   //   Output (Const)         补齐到 8 位
    0x95, 0x06,   //   Report Count (6)
    0x75, 0x08,   //   Report Size (8)
    0x15, 0x00,   //   Logical Minimum (0)
    0x25, 0x65,   //   Logical Maximum (101)
    0x05, 0x07,   //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,   //   Usage Minimum (0)
    0x29, 0x65,   //   Usage Maximum (101)
    0x81, 0x00,   //   Input (Data,Array,Abs) 第 2..7 字节：同时按下的最多 6 个键
    0xC0,         // End Collection
};

// HID 描述符：夹在接口描述符和端点描述符之间，告诉主机报告描述符有多长。
static Bytes hid_descriptor(const Bytes& report)
{
    Bytes d{9, desc::HID, 0x11, 0x01 /* HID 1.11 */, 0 /* 国家码 */, 1, desc::HID_REPORT};
    put_u16(d, static_cast<uint16_t>(report.size()));
    return d;
}

static DeviceDescriptor keyboard_dd()
{
    DeviceDescriptor dd;
    dd.idVendor = 0xCAFE;
    dd.idProduct = 0x0007;
    dd.iManufacturer = 1;
    dd.iProduct = 2;
    return dd;
}

namespace hidreq {
constexpr uint8_t GET_REPORT = 0x01, GET_IDLE = 0x02, GET_PROTOCOL = 0x03, SET_REPORT = 0x09,
                  SET_IDLE = 0x0A, SET_PROTOCOL = 0x0B;
}

class Keyboard : public Device {
public:
    Keyboard()
        : Device("键盘", keyboard_dd(),
                 ConfigBuilder(1, 100, 0xA0 /* 支持远程唤醒 */)
                     .interface(0, 0, 0x03 /* HID */, 0x01 /* 启动设备 */, 0x01 /* 键盘 */)
                     .raw(hid_descriptor(kKeyboardReport))
                     .endpoint(0x81, TransferType::Interrupt, 8, 10)
                     .build(),
                 {"miniusb 教程", "迷你键盘"})
    {
    }

    // 每个字符变成两份报告：按下、松开。连续两个相同字母中间必须有“松开”，
    // 否则主机只会认为这个键一直按着。
    void type(const std::string& text)
    {
        for (char c : text) {
            const hid::Key* k = hid::find_key(c);
            if (!k) {
                continue;
            }
            Bytes press(8, 0);
            press[0] = k->shift ? 0x02 : 0x00;   // bit1 = 左 Shift
            press[2] = k->usage;
            reports_.push_back(press);
            reports_.push_back(Bytes(8, 0));
        }
    }

    // 固件主循环：上一份报告被主机取走了，就放下一份进端点。
    void task() override
    {
        if (state() == State::Configured && !reports_.empty() && !tx_busy(0x81)) {
            write(0x81, reports_.front(), false);   // 正好 8 字节 = 包长，不需要 ZLP
            reports_.pop_front();
        }
    }

protected:
    bool get_other_descriptor(const SetupPacket& s, Bytes& out) override
    {
        uint8_t type = static_cast<uint8_t>(s.wValue >> 8);
        if (type == desc::HID_REPORT) {
            out = kKeyboardReport;
            return true;
        }
        if (type == desc::HID) {
            out = hid_descriptor(kKeyboardReport);
            return true;
        }
        return false;
    }

    bool on_class_request(const SetupPacket& s, Bytes& data) override
    {
        switch (s.bRequest) {
        case hidreq::SET_IDLE:
            // 高字节 = 间隔（单位 4 ms），0 = 只在按键变化时才发报告
            std::printf("    [固件] SET_IDLE(%u)：%s\n", s.wValue >> 8,
                        (s.wValue >> 8) ? "定时重发报告" : "只有按键变化时才发报告");
            return true;
        case hidreq::SET_REPORT:
            if (!data.empty()) {
                std::printf("    [固件] 主机设置 LED：NumLock=%d CapsLock=%d ScrollLock=%d\n",
                            data[0] & 1, (data[0] >> 1) & 1, (data[0] >> 2) & 1);
            }
            return true;
        case hidreq::GET_REPORT:
            data = Bytes(8, 0);
            return true;
        case hidreq::SET_PROTOCOL:
            return true;
        case hidreq::GET_PROTOCOL:
            data = {1};   // 1 = 报告协议，0 = 启动协议
            return true;
        default:
            return false;
        }
    }

private:
    std::deque<Bytes> reports_;
};

// 在配置描述符里找 HID 描述符，取出报告描述符的长度。
static uint16_t report_length(const Bytes& cfg)
{
    for (size_t pos = 0; pos + 9 <= cfg.size() && cfg[pos] >= 2; pos += cfg[pos]) {
        if (cfg[pos + 1] == desc::HID) {
            return get_u16(&cfg[pos + 7]);
        }
    }
    return 0;
}

static std::string describe_report(const Bytes& r)
{
    std::string s = hex(r) + "  ";
    if (r[0] & 0x22) {
        s += "[Shift] ";
    }
    bool any = false;
    for (int i = 2; i < 8; i++) {
        if (r[i]) {
            char buf[32];
            char c = hid::key_to_char(r[i], r[0] & 0x22);
            std::snprintf(buf, sizeof buf, "键码 0x%02X '%s' ", r[i],
                          c == '\n' ? "回车" : std::string(1, c).c_str());
            s += buf;
            any = true;
        }
    }
    return s + (any ? "按下" : "全部松开");
}

int main()
{
    Bus bus;
    Host host(bus);
    Keyboard kb;
    bus.attach(kb);

    title("第 1 步：枚举");
    trace.level = kTransfer;
    auto e = host.enumerate(3);
    if (!e) {
        return 1;
    }
    const InterfaceInfo& itf = e->info.interfaces[0];
    const EndpointInfo& ep = itf.endpoints[0];
    std::printf("接口类 0x%02X 子类 %u 协议 %u -> 系统的 HID 驱动接手（usbhid / hidclass.sys）\n",
                itf.cls, itf.subclass, itf.protocol);
    std::printf("中断端点 0x%02X，包长 %u，每 %u ms 轮询一次\n", ep.address, ep.max_packet,
                ep.interval);

    title("第 2 步：HID 驱动读取报告描述符（发给接口，而不是设备）");
    uint16_t len = report_length(e->config);
    auto rd = host.control_in(e->address, SetupPacket::get_descriptor(desc::HID_REPORT, 0, len,
                                                                      itf.number,
                                                                      req::kRecipInterface));
    if (!rd) {
        return 1;
    }
    hid::dump_report_descriptor(*rd);

    title("第 3 步：SET_IDLE(0) —— 没有变化就别发");
    host.control_out(e->address, {0x21, hidreq::SET_IDLE, 0, itf.number, 0});

    title("第 4 步：固件敲出 \"Hello, USB!\"，主机每 10 ms 轮询一次");
    kb.type("Hello, USB!\n");
    std::string typed;
    int polls = 0, naks = 0;
    Bytes prev(8, 0);
    for (int i = 0; i < 300; i++) {
        host.next_frame();
        if (host.frame() % ep.interval != 0) {
            continue;
        }
        polls++;
        auto report = host.interrupt_in(e->address, ep.number());
        if (!report) {
            naks++;
            continue;
        }
        std::printf("  帧 %3u  %s\n", host.frame(), describe_report(*report).c_str());
        for (int k = 2; k < 8; k++) {
            uint8_t key = (*report)[k];
            bool was_down = false;
            for (int j = 2; j < 8; j++) {
                was_down = was_down || prev[j] == key;
            }
            if (key && !was_down) {
                typed += hid::key_to_char(key, (*report)[0] & 0x22);
            }
        }
        prev = *report;
    }
    std::printf("主机轮询了 %d 次，其中 %d 次设备回 NAK（没有新按键，这很正常）\n", polls, naks);
    std::printf("屏幕上出现：%s", typed.c_str());

    title("第 5 步：用户按了 Caps Lock，主机通过 SET_REPORT 让键盘点灯");
    host.control_out(e->address, {0x21, hidreq::SET_REPORT, 0x0200 /* 输出报告, ID 0 */,
                                  itf.number, 1},
                     {0x02});

    title("附：同一个解析器读一个鼠标的报告描述符");
    const Bytes mouse = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
                         0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
                         0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
                         0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
                         0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0};
    hid::dump_report_descriptor(mouse);
    std::printf("  鼠标报告 3 字节：[按键位图][X 位移][Y 位移]，X/Y 是 -127..127 的相对量（Rel）\n");
    return 0;
}
