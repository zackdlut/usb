// hid_report.hpp —— HID 报告描述符解析器（第 8 章）。
//
// 报告描述符是一串“条目”（item），每个条目的第一个字节是前缀：
//
//    bit 7..4   bit 3..2   bit 1..0
//   ┌────────┬──────────┬──────────┐
//   │  tag   │   type   │   size   │   size: 0/1/2/3 -> 后面跟 0/1/2/4 字节数据
//   └────────┴──────────┴──────────┘   type: 0 = Main 1 = Global 2 = Local
//
// Global 条目（Usage Page、Report Size、Report Count……）像“全局变量”，设置后一直有效；
// Local 条目（Usage……）只对下一个 Main 条目有效；
// Main 条目（Input、Output、Collection……）才真正定义报告里的字段。
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>

#include "miniusb/bytes.hpp"

namespace hid {

inline const char* usage_page_name(uint32_t page)
{
    switch (page) {
    case 0x01: return "Generic Desktop";
    case 0x07: return "Keyboard/Keypad";
    case 0x08: return "LEDs";
    case 0x09: return "Button";
    case 0x0C: return "Consumer";
    default: return "?";
    }
}

inline std::string usage_name(uint32_t page, uint32_t usage)
{
    if (page == 0x01) {
        switch (usage) {
        case 0x01: return "Pointer";
        case 0x02: return "Mouse";
        case 0x06: return "Keyboard";
        case 0x30: return "X";
        case 0x31: return "Y";
        case 0x38: return "Wheel";
        }
    }
    return "0x" + musb::hex(reinterpret_cast<const uint8_t*>(&usage), 1);
}

struct Layout {
    int input_bits = 0;
    int output_bits = 0;
    int feature_bits = 0;
};

// 打印每个条目的含义，并统计输入/输出报告各有多少位。
inline Layout dump_report_descriptor(const musb::Bytes& d, bool print = true)
{
    Layout layout;
    uint32_t usage_page = 0, report_size = 0, report_count = 0;
    int depth = 0;
    for (size_t pos = 0; pos < d.size();) {
        uint8_t prefix = d[pos];
        if (prefix == 0xFE) {   // 长条目，几乎没人用，跳过
            pos += 3 + (pos + 1 < d.size() ? d[pos + 1] : 0);
            continue;
        }
        static const int kSizes[] = {0, 1, 2, 4};
        int size = kSizes[prefix & 3];
        int type = (prefix >> 2) & 3;
        int tag = prefix >> 4;
        uint32_t value = 0;
        for (int i = 0; i < size && pos + 1 + i < d.size(); i++) {
            value |= static_cast<uint32_t>(d[pos + 1 + i]) << (8 * i);
        }
        // Logical Minimum/Maximum 是有符号数
        int32_t svalue = static_cast<int32_t>(value);
        if (size == 1) {
            svalue = static_cast<int8_t>(value);
        } else if (size == 2) {
            svalue = static_cast<int16_t>(value);
        }

        std::string text, note;
        if (type == 0) {   // Main
            if (tag == 0xA) {
                static const char* const kinds[] = {"Physical", "Application", "Logical"};
                text = std::string("Collection (") + (value < 3 ? kinds[value] : "?") + ")";
            } else if (tag == 0xC) {
                depth--;
                text = "End Collection";
            } else if (tag == 0x8 || tag == 0x9 || tag == 0xB) {
                const char* name = tag == 0x8 ? "Input" : tag == 0x9 ? "Output" : "Feature";
                text = std::string(name) + " (" + ((value & 1) ? "Const" : "Data") + "," +
                       ((value & 2) ? "Var" : "Array") + "," + ((value & 4) ? "Rel" : "Abs") +
                       ")";
                int bits = static_cast<int>(report_size * report_count);
                note = std::to_string(report_count) + " 个 × " + std::to_string(report_size) +
                       " 位 = " + std::to_string(bits) + " 位";
                (tag == 0x8 ? layout.input_bits
                            : tag == 0x9 ? layout.output_bits : layout.feature_bits) += bits;
            }
        } else if (type == 1) {   // Global
            switch (tag) {
            case 0x0:
                usage_page = value;
                text = std::string("Usage Page (") + usage_page_name(value) + ")";
                break;
            case 0x1: text = "Logical Minimum (" + std::to_string(svalue) + ")"; break;
            case 0x2: text = "Logical Maximum (" + std::to_string(svalue) + ")"; break;
            case 0x7:
                report_size = value;
                text = "Report Size (" + std::to_string(value) + ")";
                break;
            case 0x8: text = "Report ID (" + std::to_string(value) + ")"; break;
            case 0x9:
                report_count = value;
                text = "Report Count (" + std::to_string(value) + ")";
                break;
            default: text = "Global tag " + std::to_string(tag); break;
            }
        } else if (type == 2) {   // Local
            switch (tag) {
            case 0x0: text = "Usage (" + usage_name(usage_page, value) + ")"; break;
            case 0x1: text = "Usage Minimum (" + std::to_string(value) + ")"; break;
            case 0x2: text = "Usage Maximum (" + std::to_string(value) + ")"; break;
            default: text = "Local tag " + std::to_string(tag); break;
            }
        }

        if (print) {
            musb::Bytes raw(d.begin() + static_cast<long>(pos),
                            d.begin() + static_cast<long>(std::min(d.size(), pos + 1 + size)));
            std::string line = std::string(static_cast<size_t>(depth * 2), ' ') + text;
            std::printf("  %-9s %s %s\n", musb::hex(raw).c_str(), musb::pad(line, 40).c_str(),
                        note.empty() ? "" : ("<- " + note).c_str());
        }
        if (type == 0 && tag == 0xA) {
            depth++;
        }
        pos += 1 + static_cast<size_t>(size);
    }
    if (print) {
        std::printf("  => 输入报告 %d 位 = %d 字节，输出报告 %d 位 = %d 字节\n", layout.input_bits,
                    layout.input_bits / 8, layout.output_bits, layout.output_bits / 8);
    }
    return layout;
}

// HID 键码（Usage Page 0x07）和字符的对照，只列常用的。
struct Key {
    char ch;
    uint8_t usage;
    bool shift;
};

inline const Key* find_key(char ch)
{
    static const Key keys[] = {
        {'\n', 0x28, false}, {' ', 0x2C, false},  {'-', 0x2D, false}, {'=', 0x2E, false},
        {',', 0x36, false},  {'.', 0x37, false},  {'/', 0x38, false}, {';', 0x33, false},
        {'!', 0x1E, true},   {'@', 0x1F, true},   {'?', 0x38, true},  {':', 0x33, true},
        {'_', 0x2D, true},   {'+', 0x2E, true},
    };
    static Key dynamic;
    if (ch >= 'a' && ch <= 'z') {
        dynamic = {ch, static_cast<uint8_t>(0x04 + ch - 'a'), false};
        return &dynamic;
    }
    if (ch >= 'A' && ch <= 'Z') {
        dynamic = {ch, static_cast<uint8_t>(0x04 + ch - 'A'), true};
        return &dynamic;
    }
    if (ch >= '1' && ch <= '9') {
        dynamic = {ch, static_cast<uint8_t>(0x1E + ch - '1'), false};
        return &dynamic;
    }
    if (ch == '0') {
        dynamic = {ch, 0x27, false};
        return &dynamic;
    }
    for (const Key& k : keys) {
        if (k.ch == ch) {
            return &k;
        }
    }
    return nullptr;
}

inline char key_to_char(uint8_t usage, bool shift)
{
    if (usage >= 0x04 && usage <= 0x1D) {
        return static_cast<char>((shift ? 'A' : 'a') + usage - 0x04);
    }
    if (usage >= 0x1E && usage <= 0x26 && !shift) {
        return static_cast<char>('1' + usage - 0x1E);
    }
    if (usage == 0x27 && !shift) {
        return '0';
    }
    for (char c = 0x20; c < 0x7F; c++) {
        const Key* k = find_key(c);
        if (k && k->usage == usage && k->shift == shift) {
            return c;
        }
    }
    return usage == 0x28 ? '\n' : '?';
}

} // namespace hid
