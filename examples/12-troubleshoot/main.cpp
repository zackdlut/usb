// 第 12 章：把四种最常见的 USB 故障在模拟器里重现一遍，
// 看清“主机看到了什么”，再对上真实系统里的报错信息。
//
//   故障 1  信号/时钟有问题：设备的回答到不了主机       dmesg: error -71
//   故障 2  配置描述符的 wTotalLength 写错              dmesg: descriptor too short（Linux 会将就）
//   故障 3  固件没往 IN 端点写数据，设备一直 NAK         libusb: LIBUSB_ERROR_TIMEOUT
//   故障 4  数据翻转位不同步：写“成功”了，数据却丢了   没有任何报错
#include <cstdio>

#include "miniusb/device.hpp"
#include "miniusb/host.hpp"

using namespace musb;

static void title(const char* t)
{
    std::printf("\n========== %s ==========\n", t);
}

static std::string to_text(const Bytes& b)
{
    return std::string(b.begin(), b.end());
}

static void verdict(const char* host_saw, const char* real_world, const char* chapter)
{
    std::printf("\n  主机看到：%s\n  真实系统：%s\n  去查    ：%s\n", host_saw, real_world, chapter);
}

static DeviceDescriptor gadget_dd()
{
    DeviceDescriptor dd;
    dd.idVendor = 0xCAFE;
    dd.idProduct = 0x0012;
    dd.iManufacturer = 1;
    dd.iProduct = 2;
    return dd;
}

static Bytes gadget_config()
{
    return ConfigBuilder(1, 100)
        .interface(0, 0, 0xFF, 0x00, 0x00)
        .endpoint(0x02, TransferType::Bulk, 64)
        .endpoint(0x82, TransferType::Bulk, 64)
        .build();
}

// 一个最简单的厂商自定义设备：一对批量端点，32 字节配置描述符（和第 11 章的固件一样）。
class Gadget : public Device {
public:
    Gadget(const char* name, Bytes config = gadget_config())
        : Device(name, gadget_dd(), std::move(config), {"miniusb 教程", "排错实验品"})
    {
    }
};

// 把一个正常的设备包起来：设备照常收包、照常回答，但回答在线上被打得面目全非，
// 主机一个也解不出来。真实世界的原因：用 HSI 当 USB 时钟、D+/D- 接反、线太长、虚焊……
// 对主控制器来说，“CRC 错”“位填充错”“没回答”都是同一件事：事务出错。
class BrokenWire : public BusDevice {
public:
    explicit BrokenWire(Device& inner) : inner_(inner) {}
    const char* name() const override { return inner_.name(); }
    std::optional<Packet> on_packet(const Packet& p) override
    {
        inner_.on_packet(p);
        return std::nullopt;
    }
    void on_bus_reset() override { inner_.on_bus_reset(); }
    void task() override { inner_.task(); }

private:
    Device& inner_;
};

int main()
{
    Bus bus;
    Host host(bus);

    // ------------------------------------------------------------------
    title("故障 1：设备的回答到不了主机（时钟不准、信号不好）");
    {
        Gadget dev("时钟不准的设备");
        BrokenWire wire(dev);
        bus.attach(wire);
        trace.level = kTransaction;
        auto e = host.enumerate(1);
        bus.detach(wire);
        std::printf("\n注意设备那边：它确实“收到请求”了，自己觉得一切正常。只看设备日志是查不出来的。\n");
        verdict(e ? "（意外地成功了）" : "第一个 SETUP 就连续 3 次出错，枚举在第 2 步失败",
                "usb 1-1: device descriptor read/64, error -71（-71 = EPROTO，传输出错；有的主控制器报 -62 或 -110）",
                "第 2 章：48 MHz 时钟准不准、D+ 上拉、接线");
    }

    // ------------------------------------------------------------------
    title("故障 2：配置描述符的 wTotalLength 写错");
    {
        Bytes cfg = gadget_config();
        cfg[2] = 40;   // 实际只有 32 字节，却声称 40
        Gadget dev("总长写错的设备", cfg);
        bus.attach(dev);
        trace.level = kTransfer;
        auto e = host.enumerate(2);
        bus.detach(dev);
        verdict(e ? "（意外地成功了）"
                  : "要 40 字节，只收到 32 字节的短包，配置描述符对不上，枚举在第 7 步失败",
                "Linux 打印 config index 0 descriptor too short (expected 40, got 32)，然后将就着用这 32 字节；\n"
                "            别的主机可能直接拒绝。本模拟器按规范从严处理",
                "第 6 章：wTotalLength = 配置 + 所有接口、端点、类描述符的总和");
    }

    // ------------------------------------------------------------------
    title("故障 3：固件忘了往 IN 端点写数据");
    {
        Gadget dev("不说话的固件");
        bus.attach(dev);
        trace.level = kOff;
        host.enumerate(3);
        trace.level = kTransaction;
        std::printf("-- 主机读批量端点 0x82：\n");
        InResult r = host.bulk_in(3, 2, 64, 64);
        verdict(r.status == InResult::Timeout ? "每次 IN 都是 NAK，最后超时（不是出错！设备一直在正常回答）"
                                              : "（意外的结果）",
                "libusb_bulk_transfer 返回 LIBUSB_ERROR_TIMEOUT；内核驱动里是 -110（ETIMEDOUT）",
                "第 4 章：NAK 是“现在没数据”；第 9 章：读的长度和 ZLP");

        std::printf("\n-- 固件写了 5 字节之后再读：\n");
        dev.write(0x82, {'h', 'e', 'l', 'l', 'o'});
        host.bulk_in(3, 2, 64, 64);
        bus.detach(dev);
    }

    // ------------------------------------------------------------------
    title("故障 4：数据翻转位不同步——写“成功”了，数据却丢了");
    {
        Gadget dev("翻转位不同步的设备");
        bus.attach(dev);
        trace.level = kOff;
        host.enumerate(4);
        trace.level = kTransaction;

        std::printf("-- 先正常写一包（主机和设备的翻转位都从 DATA0 走到 DATA1）：\n");
        host.bulk_out(4, 2, {'A'}, 64);
        std::printf("   设备收到：\"%s\"\n", to_text(dev.read(0x02)).c_str());

        std::printf("\n-- 主机发 CLEAR_FEATURE(ENDPOINT_HALT)，设备把翻转位清回 DATA0，\n");
        std::printf("   但主机这边忘了清（少调了 reset_toggle），还以为下一包该发 DATA1：\n");
        trace.level = kTransfer;
        host.control_out(4, {0x02, req::CLEAR_FEATURE, req::FEATURE_ENDPOINT_HALT, 0x02, 0});
        trace.level = kTransaction;

        std::printf("\n-- 写 \"hello\"：\n");
        bool ok = host.bulk_out(4, 2, {'h', 'e', 'l', 'l', 'o'}, 64);
        std::printf("   bulk_out 返回 %s，设备收到：\"%s\"\n", ok ? "成功" : "失败",
                    to_text(dev.read(0x02)).c_str());

        std::printf("\n-- 再写 \"world\"：\n");
        host.bulk_out(4, 2, {'w', 'o', 'r', 'l', 'd'}, 64);
        std::printf("   设备收到：\"%s\"\n", to_text(dev.read(0x02)).c_str());
        bus.detach(dev);

        verdict("什么错都没有：\"hello\" 被设备当成重发的旧包，回了 ACK 然后丢掉；之后两边又对上了",
                "偶尔“少一包”，没有任何报错。libusb_clear_halt() 会同时清两边，自己发 CLEAR_FEATURE 就容易漏",
                "第 4 章：DATA0/DATA1；第 7 章：CLEAR_FEATURE 之后翻转位回到 DATA0");
    }
    return 0;
}
