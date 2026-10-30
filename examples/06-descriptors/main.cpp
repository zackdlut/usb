// 第 6 章：描述符 —— 设备怎样介绍自己。
//
// 为一个假想的“USB 温湿度记录仪”写全套描述符：
//   接口 0（厂商自定义类）
//     端点 0x81 中断 IN ：每 100 ms 报告一次当前温湿度
//     端点 0x02 批量 OUT：主机下发命令
//     端点 0x82 批量 IN ：上传历史记录
// 然后逐字节拆开看，再用解析器读回来，最后演示几个常见的错误。
#include <cstdio>

#include "miniusb/descriptors.hpp"

using namespace musb;

static void title(const char* t)
{
    std::printf("\n========== %s ==========\n", t);
}

// 把一整串配置描述符按 bLength 切开，每个描述符一行。
static void walk(const Bytes& cfg)
{
    for (size_t pos = 0; pos + 2 <= cfg.size() && cfg[pos] >= 2;) {
        uint8_t len = cfg[pos];
        Bytes one(cfg.begin() + static_cast<long>(pos),
                  cfg.begin() + static_cast<long>(std::min(cfg.size(), pos + len)));
        std::printf("  偏移 %2zu: %s <- %s\n", pos, pad(hex(one), 27).c_str(),
                    descriptor_type_name(cfg[pos + 1]));
        pos += len;
    }
}

int main()
{
    title("第 1 步：设备描述符（18 字节）");
    DeviceDescriptor dd;
    dd.bMaxPacketSize0 = 64;
    dd.idVendor = 0xCAFE;        // 自己桌上测试用；产品要向 USB-IF 申请 VID
    dd.idProduct = 0x0006;
    dd.bcdDevice = 0x0102;       // 固件版本 1.02
    dd.iManufacturer = 1;
    dd.iProduct = 2;
    dd.iSerialNumber = 3;
    Bytes dd_bytes = dd.to_bytes();
    std::printf("%s", hexdump(dd_bytes).c_str());
    std::printf("注意 idVendor=0xCAFE 在内存里是 FE CA —— USB 是小端序。\n\n");
    std::printf("%s", dump_device(dd).c_str());

    title("第 2 步：配置描述符 —— 一整串，主机一次读走");
    Bytes cfg = ConfigBuilder(1, 100)                       // 配置值 1，总线供电 100 mA
                    .interface(0, 0, 0xFF, 0x00, 0x00, 4)   // 厂商自定义类，名字是字符串 4
                    .endpoint(0x81, TransferType::Interrupt, 8, 100)
                    .endpoint(0x02, TransferType::Bulk, 64)
                    .endpoint(0x82, TransferType::Bulk, 64)
                    .build();
    std::printf("共 %zu 字节：\n%s", cfg.size(), hexdump(cfg).c_str());
    std::printf("\n按 bLength 切开：\n");
    walk(cfg);

    title("第 3 步：像 lsusb -v 一样逐字段解释");
    std::printf("%s", dump_config(cfg).c_str());

    title("第 4 步：字符串描述符（UTF-16LE）");
    const char* strings[] = {"miniusb 教程", "温湿度记录仪", "SN-0001", "传感器接口"};
    Bytes lang = langid_descriptor();
    std::printf("字符串 0（语言列表）：%s  <- 0x0409 = 英语（美国）\n", hex(lang).c_str());
    for (int i = 0; i < 4; i++) {
        Bytes s = string_descriptor(strings[i]);
        std::printf("字符串 %d \"%s\"：\n%s", i + 1, strings[i], hexdump(s).c_str());
    }
    std::printf("“温” = U+6E29，小端存成 29 6E。所以产品名可以写中文，lsusb 和 Windows 设备管理器都能显示。\n");

    title("第 5 步：解析器读回来（主机枚举时就是这么做的）");
    std::string err;
    auto info = parse_config(cfg, &err);
    if (info) {
        std::printf("配置 %u：%zu 个接口，最大电流 %u mA\n", info->value, info->interfaces.size(),
                    info->max_power_ma);
        for (const InterfaceInfo& itf : info->interfaces) {
            std::printf("  接口 %u：类 0x%02X %s\n", itf.number, itf.cls, class_name(itf.cls));
            for (const EndpointInfo& ep : itf.endpoints) {
                std::printf("    端点 0x%02X  %-3s %s  包长 %u\n", ep.address,
                            ep.is_in() ? "IN" : "OUT", pad(transfer_type_name(ep.type), 18).c_str(),
                            ep.max_packet);
            }
        }
    }

    title("第 6 步：常见错误 —— 主机会直接拒绝这样的设备");
    struct Bug {
        const char* what;
        size_t offset;
        uint8_t value;
    } bugs[] = {
        {"wTotalLength 少算了一个端点", 2, static_cast<uint8_t>(cfg.size() - 7)},
        {"bNumInterfaces 写成 2", 4, 2},
        {"bNumEndpoints 写成 2", 9 + 4, 2},
        {"第二个端点的 bLength 写成 0", 9 + 9 + 7, 0},
    };
    for (const Bug& b : bugs) {
        Bytes bad = cfg;
        bad[b.offset] = b.value;
        std::string why;
        bool ok = parse_config(bad, &why).has_value();
        std::printf("  %s -> %s\n", pad(b.what, 30).c_str(), ok ? "居然通过了？" : why.c_str());
    }
    std::printf("\n用 ConfigBuilder 生成就不会犯前三个错：这几个数字都是自动算出来的。\n");
    return 0;
}
