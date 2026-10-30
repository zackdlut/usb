// 第 10 章：用 libusb 列出电脑上的 USB 设备，并读出真实设备的描述符。
//
//   ./lsusb_lite              列出所有设备（不需要权限）
//   ./lsusb_lite cafe:4011    打开这个设备，用控制传输读原始描述符，
//                             再交给第 6 章写的 musb::dump_config() 解析
//
// 读描述符其实就是第 7 章的 GET_DESCRIPTOR 控制传输，只不过这次是操作系统
// 替我们在真实的总线上跑完了建立、数据、状态三个阶段。
#include <cstdio>
#include <cstdlib>

#include "libusb_raii.hpp"
#include "miniusb/descriptors.hpp"

static std::string read_string(libusb_device_handle* h, uint8_t index)
{
    if (index == 0) {
        return "";
    }
    unsigned char buf[256];
    int n = libusb_get_string_descriptor_ascii(h, index, buf, sizeof buf);
    return n > 0 ? std::string(reinterpret_cast<char*>(buf), static_cast<size_t>(n)) : "";
}

static void show_details(libusb_device* dev, const libusb_device_descriptor& dd)
{
    libusb_device_handle* raw = nullptr;
    int r = libusb_open(dev, &raw);
    if (r < 0) {
        std::printf("  打不开：%s\n", usb::explain(r).c_str());
        return;
    }
    usb::Handle h(raw);

    // 设备描述符：GET_DESCRIPTOR(DEVICE)，bmRequestType = 0x80
    unsigned char buf[4096];
    r = libusb_get_descriptor(h.get(), LIBUSB_DT_DEVICE, 0, buf, 18);
    usb::check(r, "GET_DESCRIPTOR(DEVICE)");
    musb::Bytes dev_bytes(buf, buf + r);
    std::printf("\n原始设备描述符（%d 字节）：\n%s", r, musb::hexdump(dev_bytes).c_str());
    if (auto parsed = musb::DeviceDescriptor::parse(dev_bytes)) {
        std::printf("%s", musb::dump_device(*parsed).c_str());
    }
    std::printf("  厂商：%s\n  产品：%s\n  序列号：%s\n",
                read_string(h.get(), dd.iManufacturer).c_str(),
                read_string(h.get(), dd.iProduct).c_str(),
                read_string(h.get(), dd.iSerialNumber).c_str());

    // 配置描述符：先读 9 字节拿到 wTotalLength，再整个读出来 —— 和第 7 章枚举一样
    for (uint8_t i = 0; i < dd.bNumConfigurations; i++) {
        r = libusb_get_descriptor(h.get(), LIBUSB_DT_CONFIG, i, buf, 9);
        usb::check(r, "GET_DESCRIPTOR(CONFIGURATION, 9)");
        uint16_t total = static_cast<uint16_t>(buf[2] | (buf[3] << 8));
        r = libusb_get_descriptor(h.get(), LIBUSB_DT_CONFIG, i, buf,
                                  total < sizeof buf ? total : sizeof buf);
        usb::check(r, "GET_DESCRIPTOR(CONFIGURATION)");
        musb::Bytes cfg(buf, buf + r);
        std::printf("\n原始配置描述符 %u（%d 字节）：\n%s\n%s", i, r, musb::hexdump(cfg).c_str(),
                    musb::dump_config(cfg).c_str());
        std::string err;
        if (!musb::parse_config(cfg, &err)) {
            std::printf("  !! miniusb 的检查没通过：%s\n", err.c_str());
        }
    }
}

int main(int argc, char** argv)
{
    int want_vid = -1, want_pid = -1;
    if (argc > 1 && std::sscanf(argv[1], "%x:%x", &want_vid, &want_pid) != 2) {
        std::fprintf(stderr, "用法：%s [vid:pid]\n", argv[0]);
        return 2;
    }
    try {
        usb::Context ctx;
        usb::DeviceList list(ctx);
        int count = 0;
        for (libusb_device* dev : list) {
            libusb_device_descriptor dd;
            usb::check(libusb_get_device_descriptor(dev, &dd), "libusb_get_device_descriptor");
            count++;
            if (want_vid >= 0 && (dd.idVendor != want_vid || dd.idProduct != want_pid)) {
                continue;
            }
            std::printf("Bus %03u Device %03u: ID %04x:%04x  %s  USB %x.%02x  类 0x%02X %s\n",
                        libusb_get_bus_number(dev), libusb_get_device_address(dev), dd.idVendor,
                        dd.idProduct, usb::speed_name(libusb_get_device_speed(dev)),
                        dd.bcdUSB >> 8, dd.bcdUSB & 0xFF, dd.bDeviceClass,
                        musb::class_name(dd.bDeviceClass));
            if (want_vid >= 0) {
                show_details(dev, dd);
                return 0;
            }
        }
        if (count == 0) {
            std::printf("一个 USB 设备都没找到。\n");
            std::printf("在 WSL2 里这是正常的：先在 Windows 上用 usbipd 把设备转进来（见第 10 章）。\n");
        }
        if (want_vid >= 0) {
            std::printf("没找到 %04x:%04x\n", want_vid, want_pid);
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "错误：%s\n", e.what());
        return 1;
    }
    return 0;
}
