// signal.hpp —— 一个包在 D+/D- 两根线上长什么样（第 2 章）。
//
//   字节 -> 比特（低位先发）-> 位填充 -> NRZI 编码 -> J/K 线路状态
//
// 全速设备的电平：J = D+ 高、D- 低；K = D+ 低、D- 高；
// SE0 = 两根线都低（包结束；持续 10 ms 以上就是总线复位）。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "bytes.hpp"

namespace musb {

using Bits = std::vector<uint8_t>; // 每个元素是一个比特，取值 0 或 1

enum class Line : char { J = 'J', K = 'K', SE0 = '0' };
using Lines = std::vector<Line>;

// 每个字节都是最低位先发。
inline Bits bytes_to_bits(const Bytes& bytes)
{
    Bits bits;
    for (uint8_t b : bytes) {
        for (int i = 0; i < 8; i++) {
            bits.push_back((b >> i) & 1);
        }
    }
    return bits;
}

inline Bytes bits_to_bytes(const Bits& bits)
{
    Bytes bytes(bits.size() / 8, 0);
    for (size_t i = 0; i < bytes.size() * 8; i++) {
        bytes[i / 8] |= static_cast<uint8_t>(bits[i] << (i % 8));
    }
    return bytes;
}

// 连续 6 个 1 之后，发送方强行插入一个 0。这样线上最多 7 个比特时间就会
// 跳变一次，接收方才能一直对准时钟。
inline Bits bit_stuff(const Bits& in)
{
    Bits out;
    int ones = 0;
    for (uint8_t bit : in) {
        out.push_back(bit);
        ones = bit ? ones + 1 : 0;
        if (ones == 6) {
            out.push_back(0);
            ones = 0;
        }
    }
    return out;
}

// 去掉填充的 0。合法的线路上不可能出现连续 7 个 1，
// 出现了就是“位填充错误”，整个包作废。
inline std::optional<Bits> bit_unstuff(const Bits& in)
{
    Bits out;
    int ones = 0;
    for (size_t i = 0; i < in.size(); i++) {
        if (ones == 6) {
            if (in[i] != 0) {
                return std::nullopt;
            }
            ones = 0;
            continue;
        }
        out.push_back(in[i]);
        ones = in[i] ? ones + 1 : 0;
    }
    return out;
}

inline Line flip(Line l)
{
    return l == Line::J ? Line::K : Line::J;
}

// NRZI：发 0 = 线路状态翻转一次，发 1 = 保持不变。
inline Lines nrzi_encode(const Bits& bits, Line start = Line::J)
{
    Lines out;
    Line cur = start;
    for (uint8_t bit : bits) {
        if (bit == 0) {
            cur = flip(cur);
        }
        out.push_back(cur);
    }
    return out;
}

inline Bits nrzi_decode(const Lines& lines, Line start = Line::J)
{
    Bits out;
    Line prev = start;
    for (Line l : lines) {
        out.push_back(l == prev ? 1 : 0);
        prev = l;
    }
    return out;
}

// SYNC 就是字节 0x80：7 个 0 加 1 个 1，在线上是 KJKJKJKK。
// 接收方靠前面交替的部分对准时钟，看到结尾的 KK 就知道“包从下一位开始”。
constexpr uint8_t kSync = 0x80;

// 把一个包变成完整的线路状态：SYNC + 填充并 NRZI 后的数据 + EOP（SE0 SE0 J）。
inline Lines to_wire(const Bytes& packet)
{
    Bytes with_sync{kSync};
    with_sync.insert(with_sync.end(), packet.begin(), packet.end());
    Lines lines = nrzi_encode(bit_stuff(bytes_to_bits(with_sync)), Line::J);
    lines.push_back(Line::SE0);
    lines.push_back(Line::SE0);
    lines.push_back(Line::J);
    return lines;
}

// to_wire() 的逆过程。真实接收器会丢弃的情况都返回 nullopt：
// 没有 SYNC、位填充错误、比特数不是 8 的整数倍。
inline std::optional<Bytes> from_wire(const Lines& lines)
{
    Lines body;
    for (Line l : lines) {
        if (l == Line::SE0) {
            break;
        }
        body.push_back(l);
    }
    auto bits = bit_unstuff(nrzi_decode(body, Line::J));
    if (!bits || bits->size() < 8 || bits->size() % 8 != 0) {
        return std::nullopt;
    }
    Bytes bytes = bits_to_bytes(*bits);
    if (bytes[0] != kSync) {
        return std::nullopt;
    }
    return Bytes(bytes.begin() + 1, bytes.end());
}

inline std::string to_string(const Bits& bits)
{
    std::string s;
    for (uint8_t b : bits) {
        s += b ? '1' : '0';
    }
    return s;
}

inline std::string to_string(const Lines& lines)
{
    std::string s;
    for (Line l : lines) {
        s += static_cast<char>(l);
    }
    return s;
}

// 像逻辑分析仪那样画出 D+ 和 D- 两行波形，每个比特时间占两个字符宽。
inline std::string waveform(const Lines& lines)
{
    std::string states = "     ", dp = " D+  ", dm = " D-  ";
    for (Line l : lines) {
        states += (l == Line::SE0) ? "0 " : std::string(1, static_cast<char>(l)) + " ";
        dp += (l == Line::J) ? "▔▔" : "▁▁";
        dm += (l == Line::K) ? "▔▔" : "▁▁";
    }
    return states + "\n" + dp + "\n" + dm + "\n";
}

} // namespace musb
