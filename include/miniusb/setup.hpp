// setup.hpp —— 控制传输的 8 字节 SETUP 数据和标准请求（第 7 章）。
//
//   偏移 字段           含义
//   0    bmRequestType  bit7 方向(0=主机→设备 1=设备→主机)
//                       bit6..5 类型(0=标准 1=类 2=厂商)
//                       bit4..0 接收者(0=设备 1=接口 2=端点)
//   1    bRequest       请求编号，如 6 = GET_DESCRIPTOR
//   2    wValue         参数，含义取决于请求
//   4    wIndex         参数，通常是接口号或端点号
//   6    wLength        数据阶段最多传多少字节，0 = 没有数据阶段
#pragma once

#include <cstdio>
#include <optional>
#include <string>

#include "bytes.hpp"

namespace musb {

namespace req {
// bmRequestType 的各个字段
constexpr uint8_t kDirIn = 0x80;
constexpr uint8_t kTypeStandard = 0x00, kTypeClass = 0x20, kTypeVendor = 0x40;
constexpr uint8_t kRecipDevice = 0x00, kRecipInterface = 0x01, kRecipEndpoint = 0x02;

// 标准请求（USB 2.0 规范第 9.4 节）
constexpr uint8_t GET_STATUS = 0, CLEAR_FEATURE = 1, SET_FEATURE = 3, SET_ADDRESS = 5,
                  GET_DESCRIPTOR = 6, SET_DESCRIPTOR = 7, GET_CONFIGURATION = 8,
                  SET_CONFIGURATION = 9, GET_INTERFACE = 10, SET_INTERFACE = 11;

// 特性选择子（CLEAR_FEATURE / SET_FEATURE 的 wValue）
constexpr uint16_t FEATURE_ENDPOINT_HALT = 0, FEATURE_REMOTE_WAKEUP = 1;
} // namespace req

namespace desc {
// 描述符类型（GET_DESCRIPTOR 的 wValue 高字节）
constexpr uint8_t DEVICE = 1, CONFIGURATION = 2, STRING = 3, INTERFACE = 4, ENDPOINT = 5,
                  DEVICE_QUALIFIER = 6, INTERFACE_ASSOCIATION = 11;
// 类定义的描述符
constexpr uint8_t HID = 0x21, HID_REPORT = 0x22, CS_INTERFACE = 0x24, CS_ENDPOINT = 0x25;
} // namespace desc

struct SetupPacket {
    uint8_t bmRequestType = 0;
    uint8_t bRequest = 0;
    uint16_t wValue = 0;
    uint16_t wIndex = 0;
    uint16_t wLength = 0;

    bool is_in() const { return bmRequestType & req::kDirIn; }
    uint8_t type() const { return bmRequestType & 0x60; }
    uint8_t recipient() const { return bmRequestType & 0x1F; }

    Bytes to_bytes() const
    {
        Bytes b{bmRequestType, bRequest};
        put_u16(b, wValue);
        put_u16(b, wIndex);
        put_u16(b, wLength);
        return b;
    }

    static std::optional<SetupPacket> from_bytes(const Bytes& b)
    {
        if (b.size() != 8) {
            return std::nullopt;
        }
        SetupPacket s;
        s.bmRequestType = b[0];
        s.bRequest = b[1];
        s.wValue = get_u16(&b[2]);
        s.wIndex = get_u16(&b[4]);
        s.wLength = get_u16(&b[6]);
        return s;
    }

    // 几个最常用请求的快捷构造
    static SetupPacket get_descriptor(uint8_t type, uint8_t index, uint16_t length,
                                      uint16_t windex = 0,
                                      uint8_t recipient = req::kRecipDevice)
    {
        return {static_cast<uint8_t>(req::kDirIn | recipient), req::GET_DESCRIPTOR,
                static_cast<uint16_t>((type << 8) | index), windex, length};
    }
    static SetupPacket set_address(uint8_t addr) { return {0x00, req::SET_ADDRESS, addr, 0, 0}; }
    static SetupPacket set_configuration(uint8_t value)
    {
        return {0x00, req::SET_CONFIGURATION, value, 0, 0};
    }
};

inline const char* descriptor_type_name(uint8_t t)
{
    switch (t) {
    case desc::DEVICE: return "DEVICE";
    case desc::CONFIGURATION: return "CONFIGURATION";
    case desc::STRING: return "STRING";
    case desc::INTERFACE: return "INTERFACE";
    case desc::ENDPOINT: return "ENDPOINT";
    case desc::DEVICE_QUALIFIER: return "DEVICE_QUALIFIER";
    case desc::INTERFACE_ASSOCIATION: return "IAD";
    case desc::HID: return "HID";
    case desc::HID_REPORT: return "HID_REPORT";
    case desc::CS_INTERFACE: return "CS_INTERFACE";
    default: return "?";
    }
}

inline std::string describe(const SetupPacket& s)
{
    static const char* const names[] = {
        "GET_STATUS", "CLEAR_FEATURE", "?", "SET_FEATURE", "?", "SET_ADDRESS",
        "GET_DESCRIPTOR", "SET_DESCRIPTOR", "GET_CONFIGURATION", "SET_CONFIGURATION",
        "GET_INTERFACE", "SET_INTERFACE"};
    char buf[96];
    if (s.type() != req::kTypeStandard) {
        std::snprintf(buf, sizeof buf, "%s请求 0x%02X wValue=0x%04X wIndex=%u wLength=%u",
                      s.type() == req::kTypeClass ? "类" : "厂商", s.bRequest, s.wValue,
                      s.wIndex, s.wLength);
        return buf;
    }
    const char* name = s.bRequest < 12 ? names[s.bRequest] : "?";
    if (s.bRequest == req::GET_DESCRIPTOR) {
        std::snprintf(buf, sizeof buf, "GET_DESCRIPTOR(%s #%u) wLength=%u",
                      descriptor_type_name(static_cast<uint8_t>(s.wValue >> 8)),
                      s.wValue & 0xFF, s.wLength);
    } else if (s.bRequest == req::SET_ADDRESS || s.bRequest == req::SET_CONFIGURATION) {
        std::snprintf(buf, sizeof buf, "%s(%u)", name, s.wValue);
    } else {
        std::snprintf(buf, sizeof buf, "%s wValue=%u wIndex=0x%02X wLength=%u", name,
                      s.wValue, s.wIndex, s.wLength);
    }
    return buf;
}

} // namespace musb
