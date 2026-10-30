// 第 5 章：四种传输类型，以及它们怎样分享每 1 ms 一帧的总线时间。
//
//   第一部分：按 USB 2.0 规范第 5 章的算法，算出全速下各类型的最大吞吐量
//   第二部分：模拟一帧帧的调度 —— 周期性传输先占位，批量传输捡剩下的
//   第三部分：在模拟总线上对比同步传输和批量传输遇到噪声时的表现
#include <cstdio>
#include <string>
#include <vector>

#include "miniusb/device.hpp"
#include "miniusb/host.hpp"

using namespace musb;

static void title(const char* t)
{
    std::printf("\n========== %s ==========\n", t);
}

// 全速 12 Mbit/s，一帧 1 ms 能传 1500 字节（还没算位填充）。
constexpr int kFrameBytes = 1500;

// 每个事务除了负载以外的开销（规范表 5-4 ~ 5-10）：
// 令牌包、数据包的 PID 和 CRC、握手包、各自的 SYNC/EOP，以及包之间的间隔。
// 同步传输没有握手包，所以开销小一些。
int overhead(TransferType t)
{
    return t == TransferType::Isochronous ? 9 : 13;
}

static void part1()
{
    title("第一部分：全速下每种传输的理论最大吞吐量");
    std::printf("  %s     包长   每帧事务       最大吞吐量\n", pad("类型（单个端点）", 18).c_str());
    struct Row {
        TransferType type;
        int payload;
    } rows[] = {
        {TransferType::Bulk, 8},        {TransferType::Bulk, 64},
        {TransferType::Interrupt, 8},   {TransferType::Interrupt, 64},
        {TransferType::Isochronous, 192}, {TransferType::Isochronous, 1023},
    };
    for (const Row& r : rows) {
        int per_frame = kFrameBytes / (r.payload + overhead(r.type));
        if (r.type == TransferType::Isochronous && per_frame > 1) {
            per_frame = 1;   // 同步、中断端点每帧最多一个事务
        }
        if (r.type == TransferType::Interrupt) {
            per_frame = 1;
        }
        double kbps = per_frame * r.payload;   // 字节/帧 × 1000 帧/秒 ÷ 1000 = KB/s
        std::printf("  %s %8d %10d %11.0f KB/s\n", pad(transfer_type_name(r.type), 18).c_str(),
                    r.payload, per_frame, kbps);
    }
    int bulk_max = kFrameBytes / (64 + 13);
    std::printf("\n  一个批量端点一帧最多塞 %d 个 64 字节的包 = %d 字节/帧 = %d KB/s，\n", bulk_max,
                bulk_max * 64, bulk_max * 64);
    std::printf("  这就是全速 U 盘、串口芯片的速度上限（实际还要再打折扣）。\n");
    std::printf("  一个中断端点每帧只轮询一次：64 字节 × 1000 次/秒 = 64 KB/s，换来的是“延迟有保证”。\n");
}

// 第二部分：一帧帧调度
struct Stream {
    const char* name;
    char symbol;
    TransferType type;
    int payload;     // 每次事务的字节数
    int interval;    // 周期性传输：每几帧一次
};

static void part2(const std::vector<Stream>& periodic, int frames)
{
    // 周期性传输（同步 + 中断）最多占 90% 的帧，剩下的留给控制和批量。
    int reserved = 0;
    for (const Stream& s : periodic) {
        reserved += s.payload + overhead(s.type);
    }
    std::printf("周期性传输预留 %d 字节/帧（上限 %d = 90%%）", reserved, kFrameBytes * 9 / 10);
    if (reserved > kFrameBytes * 9 / 10) {
        std::printf(" -> 带宽不够，主机拒绝配置这个设备！\n");
        std::printf("（Windows 上会弹出“USB 控制器带宽不足”，常见于同一个口挂两个摄像头）\n");
        return;
    }
    std::printf("\n每个字符 = 25 字节总线时间，'S' = SOF，'.' = 空闲\n");

    long bulk_bytes = 0;
    for (int f = 0; f < frames; f++) {
        std::string bar = "S";
        int used = 5;   // SOF 包
        for (const Stream& s : periodic) {
            if (f % s.interval == 0) {
                int cost = s.payload + overhead(s.type);
                used += cost;
                bar += std::string(static_cast<size_t>((cost + 24) / 25), s.symbol);
            }
        }
        // 批量传输：一个 U 盘一直有数据要写，有空就塞 64 字节的包
        int bulk_cost = 64 + overhead(TransferType::Bulk);
        int n = 0;
        while (used + bulk_cost <= kFrameBytes) {
            used += bulk_cost;
            n++;
        }
        bulk_bytes += n * 64;
        bar += std::string(static_cast<size_t>(n * bulk_cost / 25), 'u');
        while (bar.size() < kFrameBytes / 25) {
            bar += '.';
        }
        std::printf("  帧 %2d |%s| U盘 %2d 包\n", f, bar.c_str(), n);
    }
    std::printf("U 盘平均写入速度 ≈ %.0f KB/s\n", static_cast<double>(bulk_bytes) / frames);
}

// 第三部分：同步 vs 批量，遇到噪声
class SpeakerAndDisk : public Device {
public:
    SpeakerAndDisk()
        : Device("speaker+disk", make_dd(),
                 ConfigBuilder(1)
                     .interface(0, 0, 0xFF, 0, 0)
                     .endpoint(0x01, TransferType::Isochronous, 192, 1)
                     .endpoint(0x02, TransferType::Bulk, 64)
                     .build(),
                 {})
    {
    }

private:
    static DeviceDescriptor make_dd()
    {
        DeviceDescriptor dd;
        dd.idVendor = 0xCAFE;
        dd.idProduct = 0x0005;
        return dd;
    }
};

static void part3()
{
    title("第三部分：同一段噪声，同步传输丢数据，批量传输重传");
    Bus bus;
    SpeakerAndDisk dev;
    bus.attach(dev);
    dev.debug_force_configured(1);
    Host host(bus);
    trace.level = kTransaction;

    std::printf("-- 同步 OUT：5 帧音频，每帧 192 字节，第 3 帧被打坏\n");
    bus.corrupt_next(Dir::HostToDevice, Pid::Data0, 2);
    for (int f = 0; f < 5; f++) {
        host.iso_out(1, 1, Bytes(192, static_cast<uint8_t>('0' + f)));
        host.next_frame();
    }
    Bytes audio = dev.read(0x01);
    std::printf("设备收到 %zu 字节 = %zu 帧音频（丢了的那帧不会重发，喇叭里是一声“咔”）\n",
                audio.size(), audio.size() / 192);

    std::printf("\n-- 批量 OUT：5 个 64 字节的包，第 3 个被打坏\n");
    bus.corrupt_next(Dir::HostToDevice, Pid::Data0, 1);
    host.bulk_out(1, 2, Bytes(5 * 64, 'D'), 64, false);
    std::printf("设备收到 %zu 字节，一个都不少（代价是多花了一点时间）\n", dev.read(0x02).size());
    trace.level = kTransfer;
}

int main()
{
    part1();

    title("第二部分 A：只插一个 U 盘");
    part2({}, 3);

    title("第二部分 B：再插一个键盘（中断，8 字节，每 4 ms）和一个 USB 音箱（同步，192 字节，每帧）");
    part2({{"键盘", 'k', TransferType::Interrupt, 8, 4},
           {"音箱", 'a', TransferType::Isochronous, 192, 1}},
          5);

    title("第二部分 C：再插两个摄像头（同步，每个 1023 字节/帧）");
    part2({{"键盘", 'k', TransferType::Interrupt, 8, 4},
           {"音箱", 'a', TransferType::Isochronous, 192, 1},
           {"摄像头1", 'c', TransferType::Isochronous, 1023, 1},
           {"摄像头2", 'C', TransferType::Isochronous, 1023, 1}},
          3);

    part3();
    return 0;
}
