// miniusb 的自测：用公开的已知数据检查每一层。改了库代码就跑一下：
//   cmake --build build && ./build/tests/test_miniusb
#include <cstdio>
#include <cstring>

#include "miniusb/descriptors.hpp"
#include "miniusb/packet.hpp"
#include "miniusb/signal.hpp"

using namespace musb;

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void test_crc()
{
    // CRC-16/USB 的标准校验值
    const char* s = "123456789";
    CHECK(crc16(reinterpret_cast<const uint8_t*>(s), std::strlen(s)) == 0xB4C8);
    // 零长度数据包的 CRC16 是 00 00
    CHECK(crc16(nullptr, 0) == 0x0000);
    // 抓包里随处可见的 "SETUP addr 0 ep 0" = 2D 00 10
    CHECK(encode(Packet::token(Pid::Setup, 0, 0)) == (Bytes{0x2D, 0x00, 0x10}));
    // GET_DESCRIPTOR(DEVICE, 64) 的数据包：C3 80 06 00 01 00 00 40 00 DD 94
    Bytes get_dev{0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00};
    CHECK(encode(Packet::data_packet(false, get_dev)) ==
          (Bytes{0xC3, 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00, 0xDD, 0x94}));
}

static void test_pid()
{
    CHECK(pid_byte(Pid::Ack) == 0xD2);
    CHECK(pid_byte(Pid::Nak) == 0x5A);
    CHECK(pid_byte(Pid::Stall) == 0x1E);
    CHECK(pid_byte(Pid::Data0) == 0xC3);
    CHECK(pid_byte(Pid::Data1) == 0x4B);
    CHECK(pid_byte(Pid::In) == 0x69);
    CHECK(pid_byte(Pid::Out) == 0xE1);
    CHECK(pid_byte(Pid::Sof) == 0xA5);
}

static void test_packet_roundtrip()
{
    for (uint8_t addr : {0, 1, 5, 127}) {
        for (uint8_t ep = 0; ep < 16; ep++) {
            Packet p = Packet::token(Pid::In, addr, ep);
            Decoded d = decode(encode(p));
            CHECK(d.packet && d.packet->addr == addr && d.packet->endp == ep);
        }
    }
    Decoded sof = decode(encode(Packet::sof(1234)));
    CHECK(sof.packet && sof.packet->frame == 1234);

    // 打坏任意一个比特，接收方都必须发现
    Bytes data = encode(Packet::data_packet(true, {1, 2, 3, 4, 5}));
    for (size_t i = 0; i < data.size() * 8; i++) {
        Bytes bad = data;
        bad[i / 8] ^= static_cast<uint8_t>(1u << (i % 8));
        CHECK(!decode(bad).packet);
    }
    Bytes token = encode(Packet::token(Pid::Out, 42, 3));
    for (size_t i = 0; i < token.size() * 8; i++) {
        Bytes bad = token;
        bad[i / 8] ^= static_cast<uint8_t>(1u << (i % 8));
        CHECK(!decode(bad).packet);
    }
}

static void test_signal()
{
    // SYNC 在线上是 KJKJKJKK
    Lines sync = to_wire({});
    CHECK(to_string(Lines(sync.begin(), sync.begin() + 8)) == "KJKJKJKK");
    // 位填充：连续 6 个 1 后面插一个 0
    CHECK(to_string(bit_stuff({1, 1, 1, 1, 1, 1, 1, 1})) == "111111011");
    CHECK(!bit_unstuff({1, 1, 1, 1, 1, 1, 1}));
    // 线路编解码往返
    for (const Bytes& b : {Bytes{0xD2}, Bytes{0xFF, 0xFF, 0xFF}, Bytes{0x2D, 0x00, 0x10},
                           Bytes{0x00, 0x7E, 0x3F}}) {
        auto back = from_wire(to_wire(b));
        CHECK(back && *back == b);
    }
}

static void test_descriptors()
{
    Bytes cfg = ConfigBuilder(1, 100)
                    .interface(0, 0, 0xFF, 0, 0)
                    .endpoint(0x81, TransferType::Bulk, 64)
                    .endpoint(0x01, TransferType::Bulk, 64)
                    .build();
    CHECK(cfg.size() == 9 + 9 + 7 + 7);
    CHECK(get_u16(&cfg[2]) == cfg.size());
    CHECK(cfg[4] == 1);            // bNumInterfaces
    CHECK(cfg[9 + 4] == 2);        // bNumEndpoints
    CHECK(cfg[8] == 50);           // 100 mA / 2
    std::string err;
    auto info = parse_config(cfg, &err);
    CHECK(info && info->interfaces.size() == 1 && info->interfaces[0].endpoints.size() == 2);

    Bytes broken = cfg;
    broken[2] = 30;                // 故意写错 wTotalLength
    CHECK(!parse_config(broken, &err));

    CHECK(string_from_descriptor(string_descriptor("Hi 你好")) == "Hi 你好");
    Bytes s = string_descriptor("AB");
    CHECK(s == (Bytes{6, 3, 'A', 0, 'B', 0}));

    DeviceDescriptor dd;
    dd.idVendor = 0xCAFE;
    dd.idProduct = 0x0001;
    auto back = DeviceDescriptor::parse(dd.to_bytes());
    CHECK(back && back->idVendor == 0xCAFE && back->to_bytes() == dd.to_bytes());
}

int main()
{
    test_crc();
    test_pid();
    test_packet_roundtrip();
    test_signal();
    test_descriptors();
    if (failures) {
        std::printf("%d 项检查失败\n", failures);
        return 1;
    }
    std::printf("全部通过\n");
    return 0;
}
