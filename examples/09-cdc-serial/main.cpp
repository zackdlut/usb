// 第 9 章：CDC-ACM 类 —— 做一个虚拟串口。
//
// 设备端：两个接口（通信接口 + 数据接口），用 IAD 绑在一起；
//         收到什么就转成大写回过去。
// 主机端：相当于 Linux 的 cdc_acm 驱动 + 一个终端程序：
//         设置波特率、拉高 DTR、读欢迎语、发数据、收回显。
// 最后演示“零长度包”这个最容易踩的坑。
#include <cctype>
#include <cstdio>
#include <string>

#include "miniusb/device.hpp"
#include "miniusb/host.hpp"

using namespace musb;

static void title(const char* t)
{
    std::printf("\n========== %s ==========\n", t);
}

static Bytes text(const std::string& s)
{
    return Bytes(s.begin(), s.end());
}

static std::string printable(const Bytes& b)
{
    std::string s;
    for (uint8_t c : b) {
        s += c == '\r' ? std::string("\\r") : c == '\n' ? std::string("\\n") : std::string(1, static_cast<char>(c));
    }
    return s;
}

namespace cdc {
// 类请求（CDC PSTN 子类规范 6.3 节）
constexpr uint8_t SET_LINE_CODING = 0x20, GET_LINE_CODING = 0x21, SET_CONTROL_LINE_STATE = 0x22;
// 功能描述符的子类型
constexpr uint8_t HEADER = 0x00, CALL_MANAGEMENT = 0x01, ACM = 0x02, UNION = 0x06;

struct LineCoding {
    uint32_t baud = 9600;
    uint8_t stop_bits = 0;   // 0 = 1 位，1 = 1.5 位，2 = 2 位
    uint8_t parity = 0;      // 0 = 无，1 = 奇，2 = 偶
    uint8_t data_bits = 8;

    Bytes to_bytes() const
    {
        Bytes b;
        put_u32(b, baud);
        b.push_back(stop_bits);
        b.push_back(parity);
        b.push_back(data_bits);
        return b;
    }
    static LineCoding from_bytes(const Bytes& b)
    {
        LineCoding lc;
        if (b.size() >= 7) {
            lc.baud = get_u32(b.data());
            lc.stop_bits = b[4];
            lc.parity = b[5];
            lc.data_bits = b[6];
        }
        return lc;
    }
    std::string str() const
    {
        static const char parity_ch[] = "NOEMS";
        static const char* const stops[] = {"1", "1.5", "2"};
        return std::to_string(baud) + " " + std::to_string(data_bits) +
               (parity < 5 ? parity_ch[parity] : '?') + (stop_bits < 3 ? stops[stop_bits] : "?");
    }
};
} // namespace cdc

static DeviceDescriptor serial_dd()
{
    DeviceDescriptor dd;
    // 复合设备用了 IAD，设备描述符里要写这三个“魔法数字”
    dd.bDeviceClass = 0xEF;      // Miscellaneous
    dd.bDeviceSubClass = 0x02;   // Common Class
    dd.bDeviceProtocol = 0x01;   // Interface Association Descriptor
    dd.idVendor = 0xCAFE;
    dd.idProduct = 0x0008;
    dd.iManufacturer = 1;
    dd.iProduct = 2;
    dd.iSerialNumber = 3;
    return dd;
}

static Bytes serial_config()
{
    return ConfigBuilder(1, 100)
        .iad(0, 2, 0x02, 0x02, 0x00)
        // 接口 0：通信接口。类 0x02 CDC，子类 0x02 ACM（抽象控制模型）
        .interface(0, 0, 0x02, 0x02, 0x00, 4)
        .raw({5, desc::CS_INTERFACE, cdc::HEADER, 0x10, 0x01})          // CDC 1.10
        .raw({5, desc::CS_INTERFACE, cdc::CALL_MANAGEMENT, 0x00, 0x01}) // 数据接口是 1
        .raw({4, desc::CS_INTERFACE, cdc::ACM, 0x02})                   // 支持 LINE_CODING 等请求
        .raw({5, desc::CS_INTERFACE, cdc::UNION, 0x00, 0x01})           // 主接口 0，从接口 1
        .endpoint(0x81, TransferType::Interrupt, 8, 16)                 // 串口状态通知
        // 接口 1：数据接口。类 0x0A CDC Data，一对批量端点传真正的数据
        .interface(1, 0, 0x0A, 0x00, 0x00)
        .endpoint(0x02, TransferType::Bulk, 64)
        .endpoint(0x82, TransferType::Bulk, 64)
        .build();
}

class SerialPort : public Device {
public:
    SerialPort()
        : Device("虚拟串口", serial_dd(), serial_config(),
                 {"miniusb 教程", "大写回显串口", "0001", "CDC 串口"})
    {
    }

    bool send_zlp = true;   // 第 6 步会关掉它

    // 固件主循环：DTR 刚拉高就打招呼；收到数据就转大写回显。
    void task() override
    {
        if (state() != State::Configured) {
            return;
        }
        if (dtr_ && !greeted_) {
            greeted_ = true;
            write(0x82, text("welcome! type something\r\n"), send_zlp);
        }
        if (available(0x02) && !tx_busy(0x82)) {
            Bytes in = read(0x02);
            for (uint8_t& c : in) {
                c = static_cast<uint8_t>(std::toupper(c));
            }
            write(0x82, in, send_zlp);
        }
    }

protected:
    bool on_class_request(const SetupPacket& s, Bytes& data) override
    {
        switch (s.bRequest) {
        case cdc::SET_LINE_CODING:
            coding_ = cdc::LineCoding::from_bytes(data);
            std::printf("    [固件] SET_LINE_CODING：%s（虚拟串口不在乎；USB 转 TTL 芯片要据此设置 UART）\n",
                        coding_.str().c_str());
            return true;
        case cdc::GET_LINE_CODING:
            data = coding_.to_bytes();
            return true;
        case cdc::SET_CONTROL_LINE_STATE:
            dtr_ = s.wValue & 1;
            std::printf("    [固件] SET_CONTROL_LINE_STATE：DTR=%d RTS=%d -> 终端%s\n", dtr_,
                        (s.wValue >> 1) & 1, dtr_ ? "打开了端口" : "关闭了端口");
            if (!dtr_) {
                greeted_ = false;
            }
            return true;
        default:
            return false;
        }
    }

private:
    cdc::LineCoding coding_;
    bool dtr_ = false;
    bool greeted_ = false;
};

int main()
{
    Bus bus;
    Host host(bus);
    SerialPort port;
    bus.attach(port);
    trace.level = kTransfer;

    title("第 1 步：枚举，看看 CDC 设备的配置描述符");
    trace.level = kOff;
    auto e = host.enumerate(4);
    trace.level = kTransfer;
    if (!e) {
        return 1;
    }
    std::printf("%s", dump_config(e->config).c_str());
    std::printf("-> Linux 加载 cdc_acm，出现 /dev/ttyACM0；Windows 加载 usbser.sys，出现 COMx\n");
    const uint8_t addr = e->address;
    const uint8_t itf = 0;   // 类请求发给通信接口

    title("第 2 步：终端程序打开端口：设置 115200 8N1，拉高 DTR");
    cdc::LineCoding lc;
    lc.baud = 115200;
    host.control_out(addr, {0x21, cdc::SET_LINE_CODING, 0, itf, 7}, lc.to_bytes());
    auto back = host.control_in(addr, {0xA1, cdc::GET_LINE_CODING, 0, itf, 7});
    if (back) {
        std::printf("读回来：%s\n", cdc::LineCoding::from_bytes(*back).str().c_str());
    }
    host.control_out(addr, {0x21, cdc::SET_CONTROL_LINE_STATE, 0x0003 /* DTR|RTS */, itf, 0});
    host.wait_ms(1);
    InResult r = host.bulk_in(addr, 2, 4096, 64);
    std::printf("终端显示：%s\n", printable(r.data).c_str());

    title("第 3 步：敲一行字");
    host.bulk_out(addr, 2, text("hello usb\r"), 64);
    host.wait_ms(1);
    r = host.bulk_in(addr, 2, 4096, 64);
    std::printf("终端显示：%s\n", printable(r.data).c_str());

    title("第 4 步：一次发 100 字节 —— 自动拆成 64 + 36 两个包");
    trace.level = kTransaction;
    std::string longline(100, 'x');
    host.bulk_out(addr, 2, text(longline), 64);
    host.wait_ms(1);
    r = host.bulk_in(addr, 2, 4096, 64);
    trace.level = kTransfer;

    title("第 5 步：正好 64 字节 —— 设备补了零长度包（ZLP）");
    trace.level = kTransaction;
    host.bulk_out(addr, 2, text(std::string(64, 'a')), 64);
    host.wait_ms(1);
    r = host.bulk_in(addr, 2, 4096, 64);
    std::printf("读请求%s，收到 %zu 字节\n", r.ok() ? "正常结束" : "没有结束", r.data.size());

    title("第 6 步：正好 64 字节，但设备忘了补 ZLP");
    std::printf("主机的读请求缓冲是 4096 字节。收到一个满 64 字节的包后，主机无法知道\n");
    std::printf("“后面还有没有”，只能继续 IN，而设备已经没数据了，只会回 NAK……\n");
    port.send_zlp = false;
    host.max_naks = 5;
    host.bulk_out(addr, 2, text(std::string(64, 'b')), 64);
    host.wait_ms(1);
    r = host.bulk_in(addr, 2, 4096, 64);
    std::printf("读请求%s，收到 %zu 字节。应用程序看起来就是“数据卡住了，再发点东西才吐出来”。\n",
                r.ok() ? "正常结束" : "超时", r.data.size());

    title("第 7 步：终端关闭端口");
    trace.level = kTransfer;
    host.control_out(addr, {0x21, cdc::SET_CONTROL_LINE_STATE, 0, itf, 0});
    return 0;
}
