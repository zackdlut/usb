// 第 10 章：和第 11 章的 STM32 固件对话的主机程序。
//
//   ./vendor_client info          厂商控制读 GET_INFO
//   ./vendor_client led on|off    厂商控制写 SET_LED
//   ./vendor_client echo 你好     批量 OUT 发出去，批量 IN 收回来
//   ./vendor_client bench [KB]    回环测速（默认 256 KB）
//   ./vendor_client bad           发一个设备不认识的请求，看 STALL 长什么样
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "libusb_raii.hpp"
#include "vendor_protocol.h"

static constexpr unsigned kTimeoutMs = 1000;

// bmRequestType：方向 | 类型 | 接收者，和第 7 章 setup.hpp 里的一样
static constexpr uint8_t kVendorIn = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                                     LIBUSB_RECIPIENT_DEVICE;   // 0xC0
static constexpr uint8_t kVendorOut = LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                      LIBUSB_RECIPIENT_DEVICE;  // 0x40

static usb::Handle open_device(const usb::Context& ctx)
{
    usb::Handle h(libusb_open_device_with_vid_pid(ctx.get(), VENDOR_VID, VENDOR_PID));
    if (!h) {
        throw std::runtime_error("找不到 cafe:4011（固件烧了吗？WSL 里 usbipd attach 了吗？"
                                 "有权限吗？可以先运行 lsusb_lite 看看）");
    }
    h.claim(0);
    return h;
}

static void cmd_info(usb::Handle& h)
{
    vendor_info info{};
    int n = libusb_control_transfer(h.get(), kVendorIn, VENDOR_REQ_GET_INFO, 0, 0,
                                    reinterpret_cast<unsigned char*>(&info), sizeof info,
                                    kTimeoutMs);
    usb::check(n, "GET_INFO");
    std::printf("板子      : %s\n", info.board == 1 ? "STM32F4 Discovery"
                                    : info.board == 2 ? "地阔星 STM32F103" : "?");
    std::printf("运行时间  : %.1f 秒\n", info.uptime_ms / 1000.0);
    std::printf("回环字节数: %u\n", info.loopback_bytes);
    std::printf("按键      : %s\n", info.button ? "按下" : "松开");
    std::printf("LED       : %s\n", info.led ? "亮" : "灭");
}

static void cmd_led(usb::Handle& h, bool on)
{
    int r = libusb_control_transfer(h.get(), kVendorOut, VENDOR_REQ_SET_LED, on ? 1 : 0, 0,
                                    nullptr, 0, kTimeoutMs);
    usb::check(r, "SET_LED");
    std::printf("LED 已%s\n", on ? "点亮" : "熄灭");
}

// 发出去，再把同样多的字节收回来。收的时候按“期望长度”读，
// 所以就算设备发的正好是 64 的整数倍、没有 ZLP，也不会卡住。
static void loopback(usb::Handle& h, const unsigned char* data, int len, unsigned char* back)
{
    int sent = 0;
    usb::check(libusb_bulk_transfer(h.get(), VENDOR_EP_OUT, const_cast<unsigned char*>(data), len,
                                    &sent, kTimeoutMs),
               "批量写");
    int got = 0;
    while (got < len) {
        int n = 0;
        usb::check(libusb_bulk_transfer(h.get(), VENDOR_EP_IN, back + got, len - got, &n,
                                        kTimeoutMs),
                   "批量读");
        got += n;
    }
}

static void cmd_echo(usb::Handle& h, const std::string& text)
{
    std::vector<unsigned char> back(text.size());
    loopback(h, reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()),
             back.data());
    std::printf("发送 %zu 字节：%s\n收回 %zu 字节：%.*s\n", text.size(), text.c_str(), back.size(),
                static_cast<int>(back.size()), reinterpret_cast<char*>(back.data()));
}

static void cmd_bench(usb::Handle& h, int kb)
{
    // 每次 256 字节：比设备的 512 字节发送缓冲小，一来一回不会互相等死。
    constexpr int kChunk = 256;
    std::vector<unsigned char> out(kChunk), in(kChunk);
    long total = static_cast<long>(kb) * 1024;
    auto t0 = std::chrono::steady_clock::now();
    for (long done = 0; done < total; done += kChunk) {
        for (int i = 0; i < kChunk; i++) {
            out[i] = static_cast<unsigned char>((done + i) * 7);
        }
        loopback(h, out.data(), kChunk, in.data());
        if (std::memcmp(out.data(), in.data(), kChunk) != 0) {
            throw std::runtime_error("数据不一致，偏移 " + std::to_string(done));
        }
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("回环 %d KB 用时 %.2f 秒，%.1f KB/s（每个方向），数据全部正确\n", kb, s,
                total / 1024.0 / s);
    std::printf("对照第 5 章：全速批量理论上限约 1216 KB/s；这里一来一回串行等待，所以低得多。\n");
}

static void cmd_bad(usb::Handle& h)
{
    unsigned char buf[8];
    int r = libusb_control_transfer(h.get(), kVendorIn, 0x7F, 0, 0, buf, sizeof buf, kTimeoutMs);
    std::printf("请求 0x7F 的结果：%s\n", r < 0 ? usb::explain(r).c_str() : "居然成功了？");
    std::printf("再发一个正常请求，看设备有没有恢复：\n");
    cmd_info(h);
}

int main(int argc, char** argv)
{
    std::string cmd = argc > 1 ? argv[1] : "info";
    try {
        usb::Context ctx;
        usb::Handle h = open_device(ctx);
        if (cmd == "info") {
            cmd_info(h);
        } else if (cmd == "led" && argc > 2) {
            cmd_led(h, std::string(argv[2]) == "on");
        } else if (cmd == "echo" && argc > 2) {
            cmd_echo(h, argv[2]);
        } else if (cmd == "bench") {
            cmd_bench(h, argc > 2 ? std::atoi(argv[2]) : 256);
        } else if (cmd == "bad") {
            cmd_bad(h);
        } else {
            std::fprintf(stderr, "用法：%s info | led on|off | echo <文字> | bench [KB] | bad\n",
                         argv[0]);
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "错误：%s\n", e.what());
        return 1;
    }
    return 0;
}
