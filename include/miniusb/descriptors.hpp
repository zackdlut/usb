// descriptors.hpp —— 描述符：设备的“自我介绍”（第 6 章）。
//
// 所有描述符都以同样两个字节开头：
//   [0] bLength          这个描述符一共多少字节
//   [1] bDescriptorType  它是哪种描述符
// 所以主机拿到一串描述符，只要“读长度、跳过去”就能逐个遍历，
// 遇到不认识的类型也不会迷路。
//
// 配置描述符是一棵树，但在线上是平铺的一整串：
//   配置(9) ─┬─ 接口 0(9) ─┬─ [类专用描述符...]
//            │              ├─ 端点(7)
//            │              └─ 端点(7)
//            └─ 接口 1(9) ── 端点(7) ...
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "setup.hpp"

namespace musb {

enum class TransferType : uint8_t { Control = 0, Isochronous = 1, Bulk = 2, Interrupt = 3 };

inline const char* transfer_type_name(TransferType t)
{
    switch (t) {
    case TransferType::Control: return "控制 Control";
    case TransferType::Isochronous: return "同步 Isochronous";
    case TransferType::Bulk: return "批量 Bulk";
    case TransferType::Interrupt: return "中断 Interrupt";
    }
    return "?";
}

inline const char* class_name(uint8_t cls)
{
    switch (cls) {
    case 0x00: return "（由接口各自定义）";
    case 0x01: return "Audio 音频";
    case 0x02: return "CDC 通信";
    case 0x03: return "HID 人机接口";
    case 0x07: return "Printer 打印机";
    case 0x08: return "Mass Storage 大容量存储";
    case 0x09: return "Hub 集线器";
    case 0x0A: return "CDC Data 数据";
    case 0x0E: return "Video 视频";
    case 0xE0: return "Wireless 无线";
    case 0xEF: return "Miscellaneous 杂项（IAD）";
    case 0xFE: return "Application Specific（DFU 等）";
    case 0xFF: return "Vendor Specific 厂商自定义";
    default: return "?";
    }
}

// ---- 设备描述符：18 字节，每个设备只有一个 --------------------------------
struct DeviceDescriptor {
    uint16_t bcdUSB = 0x0200;        // 遵循的 USB 版本，0x0200 = USB 2.0
    uint8_t bDeviceClass = 0;        // 0 = 类别写在各个接口里（最常见）
    uint8_t bDeviceSubClass = 0;
    uint8_t bDeviceProtocol = 0;
    uint8_t bMaxPacketSize0 = 64;    // 端点 0 的最大包长：8/16/32/64
    uint16_t idVendor = 0;           // VID，向 USB-IF 购买
    uint16_t idProduct = 0;          // PID，厂商自己分配
    uint16_t bcdDevice = 0x0100;     // 设备自己的版本号
    uint8_t iManufacturer = 0;       // 字符串描述符的编号，0 = 没有
    uint8_t iProduct = 0;
    uint8_t iSerialNumber = 0;
    uint8_t bNumConfigurations = 1;

    static constexpr uint8_t kSize = 18;

    Bytes to_bytes() const
    {
        Bytes b{kSize, desc::DEVICE};
        put_u16(b, bcdUSB);
        b.push_back(bDeviceClass);
        b.push_back(bDeviceSubClass);
        b.push_back(bDeviceProtocol);
        b.push_back(bMaxPacketSize0);
        put_u16(b, idVendor);
        put_u16(b, idProduct);
        put_u16(b, bcdDevice);
        b.push_back(iManufacturer);
        b.push_back(iProduct);
        b.push_back(iSerialNumber);
        b.push_back(bNumConfigurations);
        return b;
    }

    static std::optional<DeviceDescriptor> parse(const Bytes& b)
    {
        if (b.size() < kSize || b[0] != kSize || b[1] != desc::DEVICE) {
            return std::nullopt;
        }
        DeviceDescriptor d;
        d.bcdUSB = get_u16(&b[2]);
        d.bDeviceClass = b[4];
        d.bDeviceSubClass = b[5];
        d.bDeviceProtocol = b[6];
        d.bMaxPacketSize0 = b[7];
        d.idVendor = get_u16(&b[8]);
        d.idProduct = get_u16(&b[10]);
        d.bcdDevice = get_u16(&b[12]);
        d.iManufacturer = b[14];
        d.iProduct = b[15];
        d.iSerialNumber = b[16];
        d.bNumConfigurations = b[17];
        return d;
    }
};

// ---- 配置描述符构造器 ------------------------------------------------------
// 按顺序调用 interface()/endpoint()/raw()，build() 时自动填好三个最容易
// 算错的字段：wTotalLength、bNumInterfaces、每个接口的 bNumEndpoints。
class ConfigBuilder {
public:
    // attributes：bit7 必须为 1；bit6 = 自供电；bit5 = 支持远程唤醒
    explicit ConfigBuilder(uint8_t value = 1, uint16_t max_power_ma = 100,
                           uint8_t attributes = 0x80, uint8_t iConfiguration = 0)
        : value_(value), max_power_ma_(max_power_ma), attributes_(attributes | 0x80),
          iconfig_(iConfiguration)
    {
    }

    // 接口关联描述符（IAD）：告诉主机“接下来 count 个接口属于同一个功能”，
    // 复合设备（比如 CDC 串口要用两个接口）需要它。
    ConfigBuilder& iad(uint8_t first_itf, uint8_t count, uint8_t cls, uint8_t sub,
                       uint8_t proto, uint8_t istr = 0)
    {
        body_.insert(body_.end(),
                     {8, desc::INTERFACE_ASSOCIATION, first_itf, count, cls, sub, proto, istr});
        return *this;
    }

    ConfigBuilder& interface(uint8_t number, uint8_t alt, uint8_t cls, uint8_t sub,
                             uint8_t proto, uint8_t istr = 0)
    {
        itf_numbers_.insert(number);
        num_eps_at_ = body_.size() + 4;
        body_.insert(body_.end(), {9, desc::INTERFACE, number, alt, 0 /* bNumEndpoints */, cls,
                                   sub, proto, istr});
        return *this;
    }

    // address：bit7 = 方向（1 = IN，设备→主机），bit3..0 = 端点号
    // interval：中断端点的轮询间隔（全速下单位是 1 ms 帧）
    ConfigBuilder& endpoint(uint8_t address, TransferType type, uint16_t max_packet,
                            uint8_t interval = 0)
    {
        if (num_eps_at_) {
            body_[num_eps_at_]++;
        }
        body_.insert(body_.end(), {7, desc::ENDPOINT, address, static_cast<uint8_t>(type)});
        put_u16(body_, max_packet);
        body_.push_back(interval);
        return *this;
    }

    // 类专用描述符（HID 描述符、CDC 功能描述符……）原样插入。
    ConfigBuilder& raw(const Bytes& descriptor)
    {
        body_.insert(body_.end(), descriptor.begin(), descriptor.end());
        return *this;
    }

    Bytes build() const
    {
        Bytes b{9, desc::CONFIGURATION};
        put_u16(b, static_cast<uint16_t>(9 + body_.size()));
        b.push_back(static_cast<uint8_t>(itf_numbers_.size()));
        b.push_back(value_);
        b.push_back(iconfig_);
        b.push_back(attributes_);
        b.push_back(static_cast<uint8_t>(max_power_ma_ / 2));   // 单位是 2 mA
        b.insert(b.end(), body_.begin(), body_.end());
        return b;
    }

private:
    uint8_t value_;
    uint16_t max_power_ma_;
    uint8_t attributes_;
    uint8_t iconfig_;
    Bytes body_;
    size_t num_eps_at_ = 0;
    std::set<uint8_t> itf_numbers_;
};

// ---- 字符串描述符：UTF-16LE 编码 -------------------------------------------
// 0 号字符串比较特殊：它不是文字，而是设备支持的语言 ID 列表。
inline Bytes langid_descriptor(uint16_t langid = 0x0409 /* 英语（美国） */)
{
    Bytes b{4, desc::STRING};
    put_u16(b, langid);
    return b;
}

// 接受 UTF-8 输入（可以写中文），转成 UTF-16LE。只处理基本多文种平面。
inline Bytes string_descriptor(const std::string& utf8)
{
    Bytes b{0, desc::STRING};
    for (size_t i = 0; i < utf8.size();) {
        uint8_t c = static_cast<uint8_t>(utf8[i]);
        uint32_t cp;
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1Fu) << 6) | (utf8[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0Fu) << 12) | ((utf8[i + 1] & 0x3Fu) << 6) | (utf8[i + 2] & 0x3F);
            i += 3;
        } else {
            cp = '?';
            i += 1;
        }
        put_u16(b, static_cast<uint16_t>(cp));
    }
    b[0] = static_cast<uint8_t>(b.size());
    return b;
}

// 字符串描述符 -> UTF-8
inline std::string string_from_descriptor(const Bytes& b)
{
    std::string s;
    if (b.size() < 2 || b[1] != desc::STRING) {
        return s;
    }
    size_t len = b[0] < b.size() ? b[0] : b.size();
    for (size_t i = 2; i + 1 < len; i += 2) {
        uint32_t cp = get_u16(&b[i]);
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return s;
}

// ---- 解析配置描述符 ---------------------------------------------------------
struct EndpointInfo {
    uint8_t address = 0;
    TransferType type = TransferType::Control;
    uint16_t max_packet = 0;
    uint8_t interval = 0;

    uint8_t number() const { return address & 0x0F; }
    bool is_in() const { return address & 0x80; }
};

struct InterfaceInfo {
    uint8_t number = 0, alt = 0, cls = 0, subclass = 0, protocol = 0, istr = 0;
    std::vector<EndpointInfo> endpoints;
};

struct ConfigInfo {
    uint8_t value = 0, attributes = 0, num_interfaces = 0;
    uint16_t total_length = 0, max_power_ma = 0;
    std::vector<InterfaceInfo> interfaces;
};

// 遍历并检查一整串配置描述符。出错时返回 nullopt 并在 *error 里说明原因 ——
// 这些也正是 Windows/Linux 拒绝一个设备时会检查的东西。
inline std::optional<ConfigInfo> parse_config(const Bytes& b, std::string* error = nullptr)
{
    auto fail = [&](const std::string& why) -> std::optional<ConfigInfo> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    if (b.size() < 9 || b[0] != 9 || b[1] != desc::CONFIGURATION) {
        return fail("开头不是 9 字节的配置描述符");
    }
    ConfigInfo c;
    c.total_length = get_u16(&b[2]);
    c.num_interfaces = b[4];
    c.value = b[5];
    c.attributes = b[7];
    c.max_power_ma = static_cast<uint16_t>(b[8] * 2);
    if (c.total_length != b.size()) {
        return fail("wTotalLength=" + std::to_string(c.total_length) + "，实际有 " +
                    std::to_string(b.size()) + " 字节");
    }

    std::set<uint8_t> numbers;
    std::vector<uint8_t> declared_eps;
    for (size_t pos = 9; pos < b.size();) {
        uint8_t len = b[pos];
        if (len < 2 || pos + len > b.size()) {
            return fail("偏移 " + std::to_string(pos) + " 处的 bLength=" + std::to_string(len) +
                        " 不合法");
        }
        uint8_t type = b[pos + 1];
        if (type == desc::INTERFACE && len >= 9) {
            InterfaceInfo itf;
            itf.number = b[pos + 2];
            itf.alt = b[pos + 3];
            declared_eps.push_back(b[pos + 4]);
            itf.cls = b[pos + 5];
            itf.subclass = b[pos + 6];
            itf.protocol = b[pos + 7];
            itf.istr = b[pos + 8];
            numbers.insert(itf.number);
            c.interfaces.push_back(itf);
        } else if (type == desc::ENDPOINT && len >= 7) {
            if (c.interfaces.empty()) {
                return fail("端点描述符出现在任何接口之前");
            }
            EndpointInfo ep;
            ep.address = b[pos + 2];
            ep.type = static_cast<TransferType>(b[pos + 3] & 0x3);
            ep.max_packet = get_u16(&b[pos + 4]) & 0x7FF;
            ep.interval = b[pos + 6];
            c.interfaces.back().endpoints.push_back(ep);
        }
        pos += len;
    }
    if (numbers.size() != c.num_interfaces) {
        return fail("bNumInterfaces=" + std::to_string(c.num_interfaces) + "，实际有 " +
                    std::to_string(numbers.size()) + " 个接口");
    }
    for (size_t i = 0; i < c.interfaces.size(); i++) {
        if (c.interfaces[i].endpoints.size() != declared_eps[i]) {
            return fail("接口 " + std::to_string(c.interfaces[i].number) + " 的 bNumEndpoints=" +
                        std::to_string(declared_eps[i]) + "，实际有 " +
                        std::to_string(c.interfaces[i].endpoints.size()) + " 个端点");
        }
    }
    return c;
}

// ---- 像 lsusb -v 一样打印 ---------------------------------------------------
namespace detail {
inline void field(std::string& out, int indent, const char* name, unsigned value,
                  const std::string& note = "")
{
    char buf[160];
    std::snprintf(buf, sizeof buf, "%*s%-20s %6u  %s\n", indent, "", name, value, note.c_str());
    out += buf;
}
inline void field_hex(std::string& out, int indent, const char* name, unsigned value,
                      int digits, const std::string& note = "")
{
    char num[16], buf[160];
    std::snprintf(num, sizeof num, "0x%0*X", digits, value);
    std::snprintf(buf, sizeof buf, "%*s%-20s %6s  %s\n", indent, "", name, num, note.c_str());
    out += buf;
}
inline std::string bcd(uint16_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%x.%02x", v >> 8, v & 0xFF);
    return buf;
}
} // namespace detail

inline std::string dump_device(const DeviceDescriptor& d)
{
    using namespace detail;
    std::string o = "Device Descriptor:\n";
    field(o, 2, "bLength", DeviceDescriptor::kSize);
    field(o, 2, "bDescriptorType", desc::DEVICE, "DEVICE");
    field_hex(o, 2, "bcdUSB", d.bcdUSB, 4, "USB " + bcd(d.bcdUSB));
    field(o, 2, "bDeviceClass", d.bDeviceClass, class_name(d.bDeviceClass));
    field(o, 2, "bDeviceSubClass", d.bDeviceSubClass);
    field(o, 2, "bDeviceProtocol", d.bDeviceProtocol);
    field(o, 2, "bMaxPacketSize0", d.bMaxPacketSize0, "端点 0 一包最多这么多字节");
    field_hex(o, 2, "idVendor", d.idVendor, 4);
    field_hex(o, 2, "idProduct", d.idProduct, 4);
    field_hex(o, 2, "bcdDevice", d.bcdDevice, 4, bcd(d.bcdDevice));
    field(o, 2, "iManufacturer", d.iManufacturer, "字符串编号");
    field(o, 2, "iProduct", d.iProduct, "字符串编号");
    field(o, 2, "iSerialNumber", d.iSerialNumber, "字符串编号");
    field(o, 2, "bNumConfigurations", d.bNumConfigurations);
    return o;
}

inline std::string dump_config(const Bytes& b)
{
    using namespace detail;
    std::string o;
    int indent = 2;
    for (size_t pos = 0; pos + 2 <= b.size();) {
        uint8_t len = b[pos], type = b[pos + 1];
        if (len < 2 || pos + len > b.size()) {
            o += "  !! 偏移 " + std::to_string(pos) + " 处长度非法，停止解析\n";
            break;
        }
        const uint8_t* p = &b[pos];
        if (type == desc::CONFIGURATION && len >= 9) {
            o += "Configuration Descriptor:\n";
            field(o, 2, "bLength", len);
            field(o, 2, "bDescriptorType", type, "CONFIGURATION");
            field(o, 2, "wTotalLength", get_u16(p + 2), "配置描述符 + 后面所有描述符的总长");
            field(o, 2, "bNumInterfaces", p[4]);
            field(o, 2, "bConfigurationValue", p[5], "SET_CONFIGURATION 用这个值");
            field(o, 2, "iConfiguration", p[6]);
            std::string attr = (p[7] & 0x40) ? "自供电" : "总线供电";
            if (p[7] & 0x20) {
                attr += "，支持远程唤醒";
            }
            field_hex(o, 2, "bmAttributes", p[7], 2, attr);
            field(o, 2, "bMaxPower", p[8], std::to_string(p[8] * 2) + " mA");
            indent = 2;
        } else if (type == desc::INTERFACE_ASSOCIATION && len >= 8) {
            o += "  Interface Association:\n";
            field(o, 4, "bFirstInterface", p[2]);
            field(o, 4, "bInterfaceCount", p[3]);
            field(o, 4, "bFunctionClass", p[4], class_name(p[4]));
            field(o, 4, "bFunctionSubClass", p[5]);
            field(o, 4, "bFunctionProtocol", p[6]);
        } else if (type == desc::INTERFACE && len >= 9) {
            o += "  Interface Descriptor:\n";
            field(o, 4, "bInterfaceNumber", p[2]);
            field(o, 4, "bAlternateSetting", p[3]);
            field(o, 4, "bNumEndpoints", p[4], "不含端点 0");
            field(o, 4, "bInterfaceClass", p[5], class_name(p[5]));
            field(o, 4, "bInterfaceSubClass", p[6]);
            field(o, 4, "bInterfaceProtocol", p[7]);
            field(o, 4, "iInterface", p[8]);
            indent = 4;
        } else if (type == desc::ENDPOINT && len >= 7) {
            o += "    Endpoint Descriptor:\n";
            std::string dir = std::string((p[2] & 0x80) ? "IN（设备→主机）" : "OUT（主机→设备）") +
                              "，端点 " + std::to_string(p[2] & 0xF);
            field_hex(o, 6, "bEndpointAddress", p[2], 2, dir);
            field(o, 6, "bmAttributes", p[3], transfer_type_name(static_cast<TransferType>(p[3] & 3)));
            field(o, 6, "wMaxPacketSize", get_u16(p + 4), "字节/包");
            std::string iv = (p[3] & 3) == 3 ? "每 " + std::to_string(p[6]) + " ms 轮询一次" : "";
            field(o, 6, "bInterval", p[6], iv);
        } else if (type == desc::HID && len >= 9) {
            o += "    HID Descriptor:\n";
            field_hex(o, 6, "bcdHID", get_u16(p + 2), 4, "HID " + bcd(get_u16(p + 2)));
            field(o, 6, "bCountryCode", p[4]);
            field(o, 6, "bNumDescriptors", p[5]);
            field_hex(o, 6, "bDescriptorType", p[6], 2, p[6] == desc::HID_REPORT ? "Report" : "");
            field(o, 6, "wDescriptorLength", get_u16(p + 7), "报告描述符的长度");
        } else if (type == desc::CS_INTERFACE && len >= 3) {
            static const char* const sub[] = {"Header", "Call Management", "ACM", "?", "?", "?",
                                              "Union"};
            const char* name = p[2] < 7 ? sub[p[2]] : "?";
            o += std::string(static_cast<size_t>(indent + 2), ' ') + "CDC " + name +
                 " 功能描述符: " + hex(p, len) + "\n";
        } else {
            o += std::string(static_cast<size_t>(indent + 2), ' ') + "未知描述符 type=0x" +
                 hex(&p[1], 1) + ": " + hex(p, len) + "\n";
        }
        pos += len;
    }
    return o;
}

} // namespace musb
