// 第 4 章：事务 —— 令牌、数据、握手三个包一组，以及出错后怎么恢复。
//
// 先跳过枚举，直接把一个设备设成“地址 1、已配置”，它有两个批量端点：
//   0x01  OUT（主机→设备）
//   0x81  IN （设备→主机）
// 然后在总线上制造各种状况：正常收发、NAK、包被噪声打坏、STALL。
#include <cstdio>

#include "miniusb/device.hpp"
#include "miniusb/host.hpp"

using namespace musb;

static void title(const char* t)
{
    std::printf("\n========== %s ==========\n", t);
}

static Bytes text(const char* s)
{
    return Bytes(s, s + std::char_traits<char>::length(s));
}

static DeviceDescriptor demo_device_descriptor()
{
    DeviceDescriptor dd;
    dd.idVendor = 0xCAFE;
    dd.idProduct = 0x0004;
    return dd;
}

// 一个最简单的设备：固件主循环里把 OUT 端点收到的数据取走并打印。
class DemoDevice : public Device {
public:
    DemoDevice()
        : Device("demo", demo_device_descriptor(),
                 ConfigBuilder(1)
                     .interface(0, 0, 0xFF, 0, 0)
                     .endpoint(0x01, TransferType::Bulk, 8)
                     .endpoint(0x81, TransferType::Bulk, 8)
                     .build(),
                 {})
    {
    }

    bool app_reads = true;

    void task() override
    {
        if (app_reads && available(0x01)) {
            Bytes got = read(0x01);
            std::printf("    [固件] 从端点 0x01 取走 %zu 字节: \"%.*s\"\n", got.size(),
                        static_cast<int>(got.size()), reinterpret_cast<const char*>(got.data()));
        }
    }
};

int main()
{
    Bus bus;
    DemoDevice dev;
    bus.attach(dev);
    dev.debug_force_configured(1);   // 跳过枚举，第 7 章再讲
    Host host(bus);
    const uint8_t addr = 1;

    trace.level = kPacket;

    title("第 1 步：OUT 事务 —— OUT 令牌 + DATA0 + ACK");
    host.out_transaction(addr, 1, false, text("hi"));
    bus.run_device_tasks();
    title("第 2 步：下一个 OUT 事务用 DATA1，DATA0/DATA1 轮流");
    host.out_transaction(addr, 1, true, text("usb"));
    bus.run_device_tasks();

    title("第 3 步：IN 事务 —— 设备没数据时回 NAK");
    Bytes data;
    host.in_transaction(addr, 1, false, data);

    title("第 4 步：固件准备好数据后，IN 拿到 DATA0，主机回 ACK");
    dev.write(0x81, text("pong"), false);
    host.in_transaction(addr, 1, false, data);
    std::printf("主机收到: \"%.*s\"\n", static_cast<int>(data.size()),
                reinterpret_cast<const char*>(data.data()));

    title("第 5 步：主机发的 DATA 包被噪声打坏");
    std::printf("设备 CRC 校验失败，什么都不回；主机等不到握手，判定出错并重发同一个包。\n");
    trace.level = kTransaction;
    dev.debug_force_configured(addr);   // 翻转位从 DATA0 重新开始，方便看
    host.reset_toggle(addr, 0x01);
    bus.corrupt_next(Dir::HostToDevice, Pid::Data0);
    host.bulk_out(addr, 1, text("abc"), 8);
    bus.run_device_tasks();

    title("第 6 步：设备回的 ACK 被打坏 —— 数据翻转位的用处");
    std::printf("设备其实收下了数据，但主机没看到 ACK，于是用同一个 DATA1 重发。\n");
    std::printf("设备期望的已经是 DATA0 了，一看是 DATA1 就知道是重复包：回 ACK，丢掉数据。\n");
    bus.corrupt_next(Dir::DeviceToHost, Pid::Ack);
    host.bulk_out(addr, 1, text("xyz"), 8);
    bus.run_device_tasks();
    std::printf("结果：固件只收到一次 \"xyz\"，没有重复。\n");

    title("第 7 步：IN 方向主机的 ACK 被打坏");
    std::printf("设备没收到 ACK，下次 IN 原样重发；主机发现翻转位没变，回 ACK 并丢掉重复的那份。\n");
    dev.write(0x81, text("12345678ABCD"), false);
    bus.corrupt_next(Dir::HostToDevice, Pid::Ack);
    InResult r = host.bulk_in(addr, 1, 64, 8);
    std::printf("主机最终收到 %zu 字节: \"%.*s\"\n", r.data.size(),
                static_cast<int>(r.data.size()), reinterpret_cast<const char*>(r.data.data()));

    title("第 8 步：NAK 做流量控制 —— 设备缓冲区满了");
    std::printf("设备的接收缓冲只有 8 字节，固件暂时不取数据。\n");
    dev.set_rx_capacity(0x01, 8);
    dev.app_reads = false;
    host.max_naks = 3;
    host.bulk_out(addr, 1, text("ABCDEFGH12345678"), 8, false);
    std::printf("固件开始取数据，主机重试就成功了：\n");
    dev.app_reads = true;
    host.max_naks = 50;
    host.bulk_out(addr, 1, text("12345678"), 8, false);

    title("第 9 步：STALL —— 端点被停用");
    dev.set_stall(0x81, true);
    host.bulk_in(addr, 1, 64, 8);
    std::printf("STALL 不会自己消失，主机要用控制传输 CLEAR_FEATURE(ENDPOINT_HALT) 解除（第 7 章）。\n");
    return 0;
}
