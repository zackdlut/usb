// crc.hpp —— USB 用到的两种 CRC（第 3 章）。
//
//   CRC5 ：用于令牌包，覆盖 ADDR(7 位) + ENDP(4 位) 共 11 位
//          生成多项式 x^5 + x^2 + 1              （0b00101）
//   CRC16：用于数据包，覆盖全部负载字节
//          生成多项式 x^16 + x^15 + x^2 + 1      （0x8005）
//
// 算法和硬件完全一样：移位寄存器先置全 1，按发送顺序（每个字节低位先发）
// 一位一位地喂进去，最后把寄存器取反发出去，CRC 自己是高位先发。
#pragma once

#include <cstddef>
#include <cstdint>

namespace musb {

// 把最低 width 位倒序。包里其他字段都是低位先发，唯独 CRC 是高位先发，
// 所以放进字段之前要倒一下。
inline uint32_t reverse_bits(uint32_t v, int width)
{
    uint32_t r = 0;
    for (int i = 0; i < width; i++) {
        r = (r << 1) | ((v >> i) & 1u);
    }
    return r;
}

// 返回值可以直接左移 11 位，拼到令牌包的 16 位字段（ADDR|ENDP|CRC5）里。
inline uint8_t crc5(uint16_t data11)
{
    uint32_t reg = 0x1F;
    for (int i = 0; i < 11; i++) {
        uint32_t in = (data11 >> i) & 1u;
        uint32_t top = (reg >> 4) & 1u;
        reg = (reg << 1) & 0x1F;
        if (in ^ top) {
            reg ^= 0x05;
        }
    }
    return static_cast<uint8_t>(reverse_bits(~reg & 0x1F, 5));
}

// 返回值按小端放在负载后面（低字节先发）。
inline uint16_t crc16(const uint8_t* data, size_t len)
{
    uint32_t reg = 0xFFFF;
    for (size_t n = 0; n < len; n++) {
        for (int i = 0; i < 8; i++) {
            uint32_t in = (data[n] >> i) & 1u;
            uint32_t top = (reg >> 15) & 1u;
            reg = (reg << 1) & 0xFFFF;
            if (in ^ top) {
                reg ^= 0x8005;
            }
        }
    }
    return static_cast<uint16_t>(reverse_bits(~reg & 0xFFFF, 16));
}

} // namespace musb
