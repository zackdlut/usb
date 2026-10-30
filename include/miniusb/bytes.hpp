// miniusb：一个用来学习 USB 协议的小型模拟器，只有头文件，代码以“好读”为第一目标。
// bytes.hpp —— 字节缓冲区和十六进制打印，其他文件都会用到。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace musb {

using Bytes = std::vector<uint8_t>;

inline std::string hex(const uint8_t* p, size_t n, size_t max_shown = 32)
{
    std::string s;
    char buf[4];
    for (size_t i = 0; i < n && i < max_shown; i++) {
        std::snprintf(buf, sizeof buf, "%02X", p[i]);
        if (i) {
            s += ' ';
        }
        s += buf;
    }
    if (n > max_shown) {
        s += " ...";
    }
    return s;
}

inline std::string hex(const Bytes& b, size_t max_shown = 32)
{
    return hex(b.data(), b.size(), max_shown);
}

// 每行 16 字节、右侧带 ASCII 列，和 xxd 的样子一样。
inline std::string hexdump(const Bytes& b)
{
    std::string out;
    char buf[32];
    for (size_t row = 0; row < b.size(); row += 16) {
        std::snprintf(buf, sizeof buf, "  %04zX: ", row);
        out += buf;
        for (size_t i = row; i < row + 16; i++) {
            if (i < b.size()) {
                std::snprintf(buf, sizeof buf, "%02X ", b[i]);
                out += buf;
            } else {
                out += "   ";
            }
        }
        out += ' ';
        for (size_t i = row; i < row + 16 && i < b.size(); i++) {
            out += (b[i] >= 0x20 && b[i] < 0x7F) ? static_cast<char>(b[i]) : '.';
        }
        out += '\n';
    }
    return out;
}

// 终端里一个汉字占两列，printf 的 %-20s 却按字节数对齐。这个函数按显示宽度补空格。
inline std::string pad(const std::string& s, size_t width)
{
    size_t cols = 0;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            cols += 1;
        } else if ((c & 0xC0) != 0x80) {
            cols += (c >= 0xE0) ? 2 : 1;   // 3 字节的 UTF-8 基本都是中日韩字符
        }
    }
    return cols >= width ? s : s + std::string(width - cols, ' ');
}

// USB 是小端序：所有多字节字段都是低字节在前。
inline void put_u16(Bytes& b, uint16_t v)
{
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

inline uint16_t get_u16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline void put_u32(Bytes& b, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

inline uint32_t get_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace musb
