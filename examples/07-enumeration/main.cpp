// 第 7 章：控制传输和枚举 —— 设备插上之后的头 100 毫秒发生了什么。
//
// 设备还是第 6 章的“温湿度记录仪”，额外支持两个厂商请求：
//   0x01 GET_READING  (IN)  返回 4 字节：温度×100、湿度×100（小端 int16）
//   0x02 SET_INTERVAL (OUT) 主机写 2 字节：报告间隔（毫秒）
#include <cstdio>

#include "miniusb/device.hpp"
#include "miniusb/host.hpp"

using namespace musb;

static void title(const char* t)
{
    std::printf("\n========== %s ==========\n", t);
}

static DeviceDescriptor thermometer_dd(uint8_t mps0)
{
    DeviceDescriptor dd;
    dd.bMaxPacketSize0 = mps0;
    dd.idVendor = 0xCAFE;
    dd.idProduct = 0x0006;
    dd.bcdDevice = 0x0102;
    dd.iManufacturer = 1;
    dd.iProduct = 2;
    dd.iSerialNumber = 3;
    return dd;
}

class Thermometer : public Device {
public:
    explicit Thermometer(const char* name, uint8_t mps0 = 64)
        : Device(name, thermometer_dd(mps0),
                 ConfigBuilder(1, 100)
                     .interface(0, 0, 0xFF, 0x00, 0x00, 4)
                     .endpoint(0x81, TransferType::Interrupt, 8, 100)
                     .endpoint(0x02, TransferType::Bulk, 64)
                     .endpoint(0x82, TransferType::Bulk, 64)
                     .build(),
                 {"miniusb 教程", "温湿度记录仪", "SN-0001", "传感器接口"})
    {
    }

    uint16_t interval_ms = 100;

protected:
    bool on_vendor_request(const SetupPacket& s, Bytes& data) override
    {
        if (s.bRequest == 0x01 && s.is_in()) {
            data.clear();
            put_u16(data, 2350);   // 23.50 ℃
            put_u16(data, 4120);   // 41.20 %
            return true;
        }
        if (s.bRequest == 0x02 && !s.is_in() && data.size() == 2) {
            interval_ms = get_u16(data.data());
            std::printf("    [固件] 报告间隔改为 %u ms\n", interval_ms);
            return true;
        }
        return false;   // 其他请求一律 STALL
    }
};

int main()
{
    Bus bus;
    Host host(bus);

    // ------------------------------------------------------------------
    title("第 1 部分：一次控制传输的三个阶段（逐包展示）");
    Thermometer dev("温湿度计");
    bus.attach(dev);
    trace.level = kPacket;
    host.reset_bus();
    host.control_in(0, SetupPacket::get_descriptor(desc::DEVICE, 0, 64));
    std::printf("\n设备描述符只有 18 字节，主机要了 64：设备回一个 18 字节的短包，主机就知道没有了。\n");

    // ------------------------------------------------------------------
    title("第 2 部分：完整的枚举过程");
    trace.level = kTransfer;
    auto e = host.enumerate(5);
    if (!e) {
        return 1;
    }

    title("第 3 部分：主机现在知道了什么");
    std::printf("Bus 001 Device %03u: ID %04x:%04x %s %s（序列号 %s）\n", e->address,
                e->device.idVendor, e->device.idProduct, e->manufacturer.c_str(),
                e->product.c_str(), e->serial.c_str());
    for (const InterfaceInfo& itf : e->info.interfaces) {
        std::printf("  接口 %u 类 0x%02X -> ", itf.number, itf.cls);
        if (itf.cls == 0xFF) {
            std::printf("厂商自定义类，系统没有现成驱动，要由应用程序（libusb）或厂商驱动来用\n");
        } else {
            std::printf("交给 %s 类驱动\n", class_name(itf.cls));
        }
    }

    // ------------------------------------------------------------------
    title("第 4 部分：枚举之后的控制传输");
    trace.level = kTransaction;
    std::printf("-- 厂商请求 GET_READING（控制读）\n");
    auto reading = host.control_in(5, {0xC0, 0x01, 0, 0, 4});   // 0xC0 = IN | 厂商 | 设备
    if (reading && reading->size() == 4) {
        std::printf("温度 %.2f ℃，湿度 %.2f %%\n", get_u16(reading->data()) / 100.0,
                    get_u16(reading->data() + 2) / 100.0);
    }
    std::printf("\n-- 厂商请求 SET_INTERVAL（控制写，带 2 字节数据）\n");
    Bytes iv;
    put_u16(iv, 1000);
    host.control_out(5, {0x40, 0x02, 0, 0, 2}, iv);

    std::printf("\n-- 设备不认识的请求：回 STALL，下一个 SETUP 自动恢复\n");
    host.control_in(5, {0xC0, 0x7F, 0, 0, 1});
    host.control_in(5, {0x80, req::GET_STATUS, 0, 0, 2});

    std::printf("\n-- 批量端点被 STALL 后，用 CLEAR_FEATURE(ENDPOINT_HALT) 解除\n");
    dev.set_stall(0x82, true);
    host.bulk_in(5, 2, 64, 64);
    host.control_out(5, {0x02, req::CLEAR_FEATURE, req::FEATURE_ENDPOINT_HALT, 0x82, 0});
    host.reset_toggle(5, 0x82);
    dev.write(0x82, {'O', 'K'});
    host.bulk_in(5, 2, 64, 64);

    // ------------------------------------------------------------------
    title("第 5 部分：端点 0 只有 8 字节的设备");
    std::printf("主机一开始不知道 bMaxPacketSize0，先按 64 去读。设备回了 8 字节（正好是它的包长），\n");
    std::printf("可对主机来说 8 < 64 是个短包，于是只拿到前 8 字节就结束了。\n");
    std::printf("这正是设备描述符把 bMaxPacketSize0 放在第 7 字节的原因：前 8 字节里就能读到它。\n");
    bus.detach(dev);
    Thermometer small("小包设备", 8);
    bus.attach(small);
    trace.level = kTransfer;
    auto e2 = host.enumerate(6);
    std::printf("\n枚举%s。之后主机按 8 字节一包来读，18 字节的设备描述符要分 3 个包：\n",
                e2 ? "成功" : "失败");
    trace.level = kTransaction;
    host.control_in(6, SetupPacket::get_descriptor(desc::DEVICE, 0, 18));

    // ------------------------------------------------------------------
    title("第 6 部分：经典 bug —— 收到 SET_ADDRESS 立刻改地址");
    std::printf("规范要求设备在“状态阶段完成之后”才换地址。如果收到请求就换，\n");
    std::printf("主机发到地址 0 的状态阶段 IN 就没人回答了：\n");
    bus.detach(small);
    Thermometer buggy("有bug的设备");
    buggy.bug_set_address_too_early = true;
    bus.attach(buggy);
    trace.level = kTransaction;
    auto e3 = host.enumerate(7);
    std::printf("\n枚举%s。Linux 的 dmesg 里会看到 \"device not accepting address 7, error -71\"。\n",
                e3 ? "成功（不应该）" : "失败");
    return 0;
}
