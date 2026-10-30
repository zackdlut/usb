// 第 3 章：USB 包的格式、PID 校验和两种 CRC。
//
// 手工拼出四类包（令牌、数据、握手、SOF），逐字段拆开讲解；
// 然后故意打坏一位，看接收方怎样发现。
#include <cstdio>

#include "miniusb/packet.hpp"

using namespace musb;

static void title(const char* t)
{
    std::printf("\n========== %s ==========\n", t);
}

static void show(const char* what, const Packet& p)
{
    Bytes b = encode(p);
    std::printf("%s %s 字节: %s\n", pad(what, 26).c_str(), pad(describe(p), 30).c_str(),
                hex(b).c_str());
}

int main()
{
    title("第 1 步：PID —— 低 4 位是类型，高 4 位是它的反码");
    for (Pid pid : {Pid::Out, Pid::In, Pid::Setup, Pid::Sof, Pid::Data0, Pid::Data1, Pid::Ack,
                    Pid::Nak, Pid::Stall}) {
        uint8_t b = pid_byte(pid);
        static const char* const kinds[] = {"令牌", "数据", "握手", "特殊"};
        std::printf("  %-6s 0x%X -> PID 字节 0x%02X  (高4位 0x%X = ~0x%X)  %s包\n", pid_name(pid),
                    static_cast<unsigned>(pid), b, b >> 4, b & 0xF,
                    kinds[static_cast<int>(pid_kind(pid))]);
    }

    title("第 2 步：令牌包 = PID + 7 位地址 + 4 位端点 + CRC5");
    Packet tok = Packet::token(Pid::In, 5, 1);
    Bytes tb = encode(tok);
    uint16_t field = get_u16(&tb[1]);
    std::printf("IN 令牌，地址 5，端点 1：%s\n", hex(tb).c_str());
    std::printf("  后两字节按小端拼成 16 位：0x%04X\n", field);
    std::printf("  bit 0..6   ADDR = %u\n", field & 0x7F);
    std::printf("  bit 7..10  ENDP = %u\n", (field >> 7) & 0xF);
    std::printf("  bit 11..15 CRC5 = 0x%02X  (只覆盖 ADDR+ENDP 这 11 位)\n", field >> 11);
    show("SETUP 地址 0 端点 0", Packet::token(Pid::Setup, 0, 0));
    show("OUT 地址 1 端点 2", Packet::token(Pid::Out, 1, 2));

    title("第 3 步：数据包 = PID + 负载 + CRC16");
    // 主机枚举时发的第一个请求：GET_DESCRIPTOR(DEVICE)，要 64 字节
    Bytes get_desc{0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00};
    show("DATA0 + 8 字节 SETUP 数据", Packet::data_packet(false, get_desc));
    show("DATA1 + \"ABC\"", Packet::data_packet(true, {'A', 'B', 'C'}));
    show("零长度数据包 ZLP", Packet::data_packet(true, {}));
    std::printf("  ZLP 没有负载，CRC16 = 00 00；它的意思是“这次传输到此为止”\n");

    title("第 4 步：握手包只有一个 PID 字节");
    show("ACK  我收到了", Packet::handshake(Pid::Ack));
    show("NAK  我现在忙，待会再来", Packet::handshake(Pid::Nak));
    show("STALL 出错了/不支持", Packet::handshake(Pid::Stall));

    title("第 5 步：SOF —— 每 1 ms 广播一次帧号");
    show("SOF 帧号 1234", Packet::sof(1234));

    title("第 6 步：接收方的检查");
    Bytes good = encode(Packet::data_packet(true, {'A', 'B', 'C'}));
    std::printf("完好的包：%s -> %s\n", hex(good).c_str(),
                decode(good).packet ? "通过" : decode(good).error.c_str());
    struct Case {
        const char* what;
        size_t byte;
        uint8_t mask;
    } cases[] = {
        {"PID 的第 0 位", 0, 0x01},
        {"负载 'B' 的第 3 位", 2, 0x08},
        {"CRC 的最高位", 5, 0x80},
    };
    for (const Case& c : cases) {
        Bytes bad = good;
        bad[c.byte] ^= c.mask;
        Decoded d = decode(bad);
        std::printf("打坏 %s %s -> %s\n", pad(c.what, 18).c_str(), hex(bad).c_str(),
                    d.packet ? "居然通过了？！" : d.error.c_str());
    }
    Bytes t = encode(Packet::token(Pid::Out, 1, 2));
    t[1] ^= 0x02;   // 地址 1 变成地址 3
    std::printf("%s %s -> %s\n", pad("令牌地址被打成 3", 23).c_str(), hex(t).c_str(),
                decode(t).error.c_str());
    std::printf("\n被丢弃的包不会有任何回应。发送方等不到握手，就知道要重发（第 4 章）。\n");
    return 0;
}
