// trace.hpp —— 分层打印日志。级别越高看到的细节越多：
//
//   1 传输（transfer）    "控制读 GET_DESCRIPTOR(DEVICE) -> 18 字节"
//   2 事务（transaction） "SETUP  addr=0 ep=0 DATA0 [8] -> ACK"
//   3 包（packet）        "主机→设备  SETUP addr=0 ep=0   | 2D 00 10"
//   4 线路（wire）        "KJKJKJKK..." 每个包在 D+/D- 上的样子
#pragma once

#include <cstdarg>
#include <cstdio>

namespace musb {

enum Level { kOff = 0, kTransfer = 1, kTransaction = 2, kPacket = 3, kWire = 4 };

struct Trace {
    int level = kTransfer;
    bool show_sof = false;   // SOF 每 1 ms 一个，默认不打印，免得刷屏
};

inline Trace trace;

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
inline void log(int lvl, const char* fmt, ...)
{
    if (trace.level < lvl) {
        return;
    }
    static const char* const indent[] = {"", "", "  ", "    ", "      "};
    std::fputs(indent[lvl < 0 ? 0 : (lvl > 4 ? 4 : lvl)], stdout);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
}

} // namespace musb
