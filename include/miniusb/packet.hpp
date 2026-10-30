// packet.hpp —— USB 包：PID、令牌包 / 数据包 / 握手包 / SOF（第 3 章）。
//
//   令牌包   : PID | ADDR(7) ENDP(4) CRC5(5)          共 3 字节
//   SOF 包   : PID | 帧号(11) CRC5(5)                 共 3 字节
//   数据包   : PID | 负载 0..1023 字节 | CRC16        共 3+N 字节
//   握手包   : PID                                    共 1 字节
#pragma once

#include <optional>
#include <string>

#include "bytes.hpp"
#include "crc.hpp"

namespace musb {

// PID 字节的低 4 位。高 4 位是低 4 位取反，所以 PID 错一位一定能被发现。
enum class Pid : uint8_t {
    Out = 0x1, In = 0x9, Sof = 0x5, Setup = 0xD,              // 令牌
    Data0 = 0x3, Data1 = 0xB, Data2 = 0x7, MData = 0xF,       // 数据
    Ack = 0x2, Nak = 0xA, Stall = 0xE, Nyet = 0x6,            // 握手
    Pre = 0xC, Split = 0x8, Ping = 0x4,                       // 特殊（Hub、高速用）
};

enum class PidKind { Token, Data, Handshake, Special };

inline PidKind pid_kind(Pid pid)
{
    // PID 的最低两位就能区分包的大类。
    switch (static_cast<uint8_t>(pid) & 0x3) {
    case 0x1: return PidKind::Token;
    case 0x3: return PidKind::Data;
    case 0x2: return PidKind::Handshake;
    default:  return PidKind::Special;
    }
}

inline const char* pid_name(Pid pid)
{
    switch (pid) {
    case Pid::Out:   return "OUT";
    case Pid::In:    return "IN";
    case Pid::Sof:   return "SOF";
    case Pid::Setup: return "SETUP";
    case Pid::Data0: return "DATA0";
    case Pid::Data1: return "DATA1";
    case Pid::Data2: return "DATA2";
    case Pid::MData: return "MDATA";
    case Pid::Ack:   return "ACK";
    case Pid::Nak:   return "NAK";
    case Pid::Stall: return "STALL";
    case Pid::Nyet:  return "NYET";
    case Pid::Pre:   return "PRE";
    case Pid::Split: return "SPLIT";
    case Pid::Ping:  return "PING";
    }
    return "?";
}

inline uint8_t pid_byte(Pid pid)
{
    uint8_t p = static_cast<uint8_t>(pid);
    return static_cast<uint8_t>(p | ((~p & 0xF) << 4));
}

inline Pid data_pid(bool toggle)
{
    return toggle ? Pid::Data1 : Pid::Data0;
}

struct Packet {
    Pid pid = Pid::Ack;
    uint8_t addr = 0;     // 令牌包：设备地址 0..127
    uint8_t endp = 0;     // 令牌包：端点号 0..15
    uint16_t frame = 0;   // SOF：帧号 0..2047
    Bytes data;           // 数据包：负载

    static Packet token(Pid pid, uint8_t addr, uint8_t endp)
    {
        Packet p;
        p.pid = pid;
        p.addr = addr & 0x7F;
        p.endp = endp & 0x0F;
        return p;
    }
    static Packet sof(uint16_t frame)
    {
        Packet p;
        p.pid = Pid::Sof;
        p.frame = frame & 0x7FF;
        return p;
    }
    static Packet data_packet(bool toggle, Bytes payload)
    {
        Packet p;
        p.pid = data_pid(toggle);
        p.data = std::move(payload);
        return p;
    }
    static Packet handshake(Pid pid)
    {
        Packet p;
        p.pid = pid;
        return p;
    }

    PidKind kind() const { return pid_kind(pid); }
};

// 包 -> 字节（SYNC 和 EOP 之间的部分）。
inline Bytes encode(const Packet& p)
{
    Bytes b{pid_byte(p.pid)};
    if (p.pid == Pid::Sof) {
        put_u16(b, static_cast<uint16_t>(p.frame | (crc5(p.frame) << 11)));
    } else if (p.kind() == PidKind::Token) {
        uint16_t data11 = static_cast<uint16_t>(p.addr | (p.endp << 7));
        put_u16(b, static_cast<uint16_t>(data11 | (crc5(data11) << 11)));
    } else if (p.kind() == PidKind::Data) {
        b.insert(b.end(), p.data.begin(), p.data.end());
        put_u16(b, crc16(p.data.data(), p.data.size()));
    }
    return b;
}

struct Decoded {
    std::optional<Packet> packet;
    std::string error;   // 接收方丢弃这个包的原因
};

// 字节 -> 包，做了 USB 接收方要做的全部检查。
inline Decoded decode(const Bytes& b)
{
    if (b.empty()) {
        return {std::nullopt, "空包"};
    }
    uint8_t lo = b[0] & 0xF, hi = b[0] >> 4;
    if ((lo ^ hi) != 0xF) {
        return {std::nullopt, "PID 校验位不对"};
    }
    Packet p;
    p.pid = static_cast<Pid>(lo);

    if (p.kind() == PidKind::Token) {
        if (b.size() != 3) {
            return {std::nullopt, "令牌包必须是 3 字节"};
        }
        uint16_t field = get_u16(&b[1]);
        uint16_t data11 = field & 0x7FF;
        if (crc5(data11) != (field >> 11)) {
            return {std::nullopt, "CRC5 错误"};
        }
        if (p.pid == Pid::Sof) {
            p.frame = data11;
        } else {
            p.addr = data11 & 0x7F;
            p.endp = (data11 >> 7) & 0xF;
        }
    } else if (p.kind() == PidKind::Data) {
        if (b.size() < 3) {
            return {std::nullopt, "数据包太短"};
        }
        p.data.assign(b.begin() + 1, b.end() - 2);
        if (crc16(p.data.data(), p.data.size()) != get_u16(&b[b.size() - 2])) {
            return {std::nullopt, "CRC16 错误"};
        }
    } else if (p.kind() == PidKind::Handshake) {
        if (b.size() != 1) {
            return {std::nullopt, "握手包必须是 1 字节"};
        }
    }
    return {p, ""};
}

// 一行可读的描述，例如 "SETUP addr=0 ep=0"、"DATA1 [3] 41 42 43"。
inline std::string describe(const Packet& p)
{
    std::string s = pid_name(p.pid);
    if (p.pid == Pid::Sof) {
        s += " frame=" + std::to_string(p.frame);
    } else if (p.kind() == PidKind::Token) {
        s += " addr=" + std::to_string(p.addr) + " ep=" + std::to_string(p.endp);
    } else if (p.kind() == PidKind::Data) {
        s += " [" + std::to_string(p.data.size()) + "]";
        if (!p.data.empty()) {
            s += " " + hex(p.data, 16);
        }
    }
    return s;
}

} // namespace musb
