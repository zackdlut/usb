// host.hpp —— 主机端协议栈（第 4、5、7 章）。在 PC 上这部分是 USB 主控制器
// 硬件（xHCI/EHCI）+ 操作系统的 USB 核心驱动。
//
// 自底向上三层：
//   事务（transaction）：一次“令牌 + 数据 + 握手”，失败就最多重试 3 次
//   传输（transfer）    ：控制 / 批量 / 中断 / 同步，由一个或多个事务组成
//   枚举（enumeration） ：插入新设备后的一连串控制传输
#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "bus.hpp"
#include "descriptors.hpp"
#include "setup.hpp"

namespace musb {

// 一次事务的结果
enum class Result {
    Ack,        // 成功
    Nak,        // 设备忙 / 没数据，稍后再试
    Stall,      // 设备说“这个请求我不支持”或端点被停用
    Error,      // 没回答、CRC 错……总之没收到一个完好的回答
    Duplicate,  // （仅 IN）收到的是上次已经收过的数据，已回 ACK 并丢弃
};

inline const char* result_name(Result r)
{
    switch (r) {
    case Result::Ack: return "ACK";
    case Result::Nak: return "NAK";
    case Result::Stall: return "STALL";
    case Result::Error: return "超时/出错";
    case Result::Duplicate: return "重复数据";
    }
    return "?";
}

// 批量/中断读的结果
struct InResult {
    enum Status { Ok, Stall, Timeout, Error } status = Error;
    Bytes data;
    bool ok() const { return status == Ok; }
};

class Host {
public:
    explicit Host(Bus& bus) : bus_(bus) {}

    int max_errors = 3;   // 规范要求：同一个事务连续出错 3 次才放弃
    int max_naks = 50;    // 模拟器里用来代替“超时时间”

    // ---- 帧 ------------------------------------------------------------------
    // 全速总线把时间切成 1 ms 一帧，每帧开头主机发一个 SOF。
    void next_frame()
    {
        bus_.sof(frame_);
        frame_ = static_cast<uint16_t>((frame_ + 1) & 0x7FF);
        bus_.run_device_tasks();
    }
    void wait_ms(int ms)
    {
        for (int i = 0; i < ms; i++) {
            next_frame();
        }
    }
    uint16_t frame() const { return frame_; }

    // 复位期间总线上是 SE0，没有 SOF。
    void reset_bus()
    {
        bus_.reset();
        toggles_.clear();
        mps0_.clear();
        for (int i = 0; i < 10; i++) {
            bus_.run_device_tasks();
        }
    }

    // ---- 事务层 ---------------------------------------------------------------
    // SETUP 事务：SETUP 令牌 + DATA0（8 字节）+ 设备回 ACK
    Result setup_transaction(uint8_t addr, const Bytes& setup8)
    {
        return out_like(Pid::Setup, addr, 0, false, setup8);
    }

    // OUT 事务：OUT 令牌 + DATAx + 设备回 ACK/NAK/STALL
    Result out_transaction(uint8_t addr, uint8_t ep, bool toggle, const Bytes& data)
    {
        return out_like(Pid::Out, addr, ep, toggle, data);
    }

    // IN 事务：IN 令牌 + 设备回 DATAx（或 NAK/STALL）+ 主机回 ACK
    Result in_transaction(uint8_t addr, uint8_t ep, bool toggle, Bytes& data)
    {
        auto reply = bus_.host_send(Packet::token(Pid::In, addr, ep));
        Result r;
        if (!reply) {
            r = Result::Error;
        } else if (reply->pid == Pid::Nak) {
            r = Result::Nak;
        } else if (reply->pid == Pid::Stall) {
            r = Result::Stall;
        } else if (reply->kind() == PidKind::Data) {
            // 不管翻转位对不对，主机都回 ACK；不对说明是重发的旧数据，丢掉。
            bus_.host_send(Packet::handshake(Pid::Ack));
            if ((reply->pid == Pid::Data1) == toggle) {
                data = reply->data;
                r = Result::Ack;
            } else {
                r = Result::Duplicate;
            }
        } else {
            r = Result::Error;
        }
        log_transaction("IN   ", addr, ep,
                        reply && reply->kind() == PidKind::Data ? describe(*reply) + " ←" : "→", r);
        return r;
    }

    // ---- 传输层：控制传输 ---------------------------------------------------
    // 控制读：建立阶段（SETUP）→ 数据阶段（若干 IN）→ 状态阶段（一个零长度 OUT）
    std::optional<Bytes> control_in(uint8_t addr, const SetupPacket& s)
    {
        log(kTransaction, "── 建立阶段");
        if (retry([&] { return setup_transaction(addr, s.to_bytes()); }) != Result::Ack) {
            return fail_control(s, "SETUP 没有被确认");
        }
        log(kTransaction, "── 数据阶段（设备→主机）");
        uint16_t mps = mps0(addr);
        Bytes result;
        bool toggle = true;
        int errors = 0, naks = 0;
        while (result.size() < s.wLength) {
            Bytes chunk;
            Result r = in_transaction(addr, 0, toggle, chunk);
            if (r == Result::Ack) {
                result.insert(result.end(), chunk.begin(), chunk.end());
                toggle = !toggle;
                if (chunk.size() < mps) {
                    break;   // 短包：设备说“我就这么多了”
                }
            } else if (r == Result::Stall) {
                return fail_control(s, "设备回了 STALL");
            } else if (r == Result::Nak) {
                if (++naks > max_naks) {
                    return fail_control(s, "设备一直 NAK");
                }
                bus_.run_device_tasks();
            } else if (r == Result::Error && ++errors >= max_errors) {
                return fail_control(s, "连续 3 次出错");
            }
        }
        log(kTransaction, "── 状态阶段（主机发零长度 OUT 表示“收到了”）");
        Result r = retry([&] { return out_transaction(addr, 0, true, {}); });
        if (r != Result::Ack) {
            return fail_control(s, std::string("状态阶段 ") + result_name(r));
        }
        flush_nak_log();
        log(kTransfer, "控制读  addr=%u %s → %zu 字节", addr, describe(s).c_str(), result.size());
        return result;
    }

    // 控制写：建立阶段 → 数据阶段（若干 OUT，可以没有）→ 状态阶段（一个零长度 IN）
    bool control_out(uint8_t addr, const SetupPacket& s, const Bytes& data = {})
    {
        log(kTransaction, "── 建立阶段");
        if (retry([&] { return setup_transaction(addr, s.to_bytes()); }) != Result::Ack) {
            fail_control(s, "SETUP 没有被确认");
            return false;
        }
        uint16_t mps = mps0(addr);
        bool toggle = true;
        if (!data.empty()) {
            log(kTransaction, "── 数据阶段（主机→设备）");
        }
        for (size_t i = 0; i < data.size(); i += mps) {
            Bytes chunk(data.begin() + static_cast<long>(i),
                        data.begin() + static_cast<long>(std::min(data.size(), i + mps)));
            Result r = retry([&] { return out_transaction(addr, 0, toggle, chunk); });
            if (r != Result::Ack) {
                fail_control(s, std::string("数据阶段 ") + result_name(r));
                return false;
            }
            toggle = !toggle;
        }
        log(kTransaction, "── 状态阶段（设备回零长度 DATA1 表示“处理完了”）");
        Result r = retry([&] {
            Bytes zlp;
            return in_transaction(addr, 0, true, zlp);
        });
        if (r != Result::Ack) {
            fail_control(s, std::string("状态阶段 ") + result_name(r));
            return false;
        }
        flush_nak_log();
        log(kTransfer, "控制写  addr=%u %s → 完成", addr, describe(s).c_str());
        return true;
    }

    // ---- 传输层：批量 / 中断 / 同步 -----------------------------------------
    // 批量写：把数据切成最大包长的块逐个 OUT。长度正好是包长整数倍时，
    // zlp = true 会补一个零长度包，告诉设备“传完了”。
    bool bulk_out(uint8_t addr, uint8_t ep, const Bytes& data, uint16_t mps, bool zlp = true)
    {
        std::vector<Bytes> packets;
        for (size_t i = 0; i < data.size(); i += mps) {
            size_t n = std::min<size_t>(mps, data.size() - i);
            packets.emplace_back(data.begin() + static_cast<long>(i),
                                 data.begin() + static_cast<long>(i + n));
        }
        if (zlp && data.size() % mps == 0) {
            packets.emplace_back();   // 零长度包
        }
        bool& toggle = toggles_[key(addr, ep)];
        for (const Bytes& chunk : packets) {
            Result r = retry([&] { return out_transaction(addr, ep, toggle, chunk); });
            if (r != Result::Ack) {
                flush_nak_log();
                log(kTransfer, "批量写  addr=%u ep=%u 失败：%s", addr, ep, result_name(r));
                return false;
            }
            toggle = !toggle;
        }
        flush_nak_log();
        log(kTransfer, "批量写  addr=%u ep=%u ← %zu 字节（%zu 个包）", addr, ep, data.size(),
            packets.size());
        return true;
    }

    // 批量读：一直 IN，直到收到短包（含零长度包）或者读满 max_len。
    InResult bulk_in(uint8_t addr, uint8_t ep, size_t max_len, uint16_t mps)
    {
        bool& toggle = toggles_[key(addr, ep | 0x80)];
        InResult res;
        int errors = 0, naks = 0;
        while (res.data.size() < max_len) {
            Bytes chunk;
            Result r = in_transaction(addr, ep, toggle, chunk);
            if (r == Result::Ack) {
                res.data.insert(res.data.end(), chunk.begin(), chunk.end());
                toggle = !toggle;
                naks = 0;
                if (chunk.size() < mps) {
                    break;
                }
            } else if (r == Result::Nak) {
                if (++naks > max_naks) {
                    flush_nak_log();
                    res.status = InResult::Timeout;
                    log(kTransfer, "批量读  addr=%u ep=%u 超时：已收到 %zu 字节，但一直等不到短包", addr,
                        ep, res.data.size());
                    return res;
                }
                bus_.run_device_tasks();
            } else if (r == Result::Stall) {
                res.status = InResult::Stall;
                flush_nak_log();
                log(kTransfer, "批量读  addr=%u ep=%u 端点被 STALL", addr, ep);
                return res;
            } else if (r == Result::Error && ++errors >= max_errors) {
                res.status = InResult::Error;
                return res;
            }
        }
        res.status = InResult::Ok;
        flush_nak_log();
        log(kTransfer, "批量读  addr=%u ep=%u → %zu 字节", addr, ep, res.data.size());
        return res;
    }

    // 中断读：只做一次 IN 事务。NAK 很正常，意思是“这段时间没有新情况”。
    std::optional<Bytes> interrupt_in(uint8_t addr, uint8_t ep)
    {
        bool& toggle = toggles_[key(addr, ep | 0x80)];
        Bytes data;
        Result r = in_transaction(addr, ep, toggle, data);
        if (r != Result::Ack) {
            return std::nullopt;
        }
        toggle = !toggle;
        return data;
    }

    // 同步写/读：没有握手、没有重试，出错就丢。
    void iso_out(uint8_t addr, uint8_t ep, const Bytes& data)
    {
        bus_.host_send(Packet::token(Pid::Out, addr, ep));
        bus_.host_send(Packet::data_packet(false, data));
        log_transaction("OUT  ", addr, ep, "DATA0 [" + std::to_string(data.size()) + "]", std::nullopt);
    }
    std::optional<Bytes> iso_in(uint8_t addr, uint8_t ep)
    {
        auto reply = bus_.host_send(Packet::token(Pid::In, addr, ep));
        log_transaction("IN   ", addr, ep, reply ? describe(*reply) + " ←" : "（丢了）", std::nullopt);
        if (!reply || reply->kind() != PidKind::Data) {
            return std::nullopt;
        }
        return reply->data;
    }

    // 清除端点 HALT 之后，主机这边的翻转位也要回到 DATA0。
    void reset_toggle(uint8_t addr, uint8_t ep_addr) { toggles_[key(addr, ep_addr)] = false; }

    // ---- 枚举 ---------------------------------------------------------------
    struct Enumerated {
        uint8_t address = 0;
        DeviceDescriptor device;
        Bytes config;
        ConfigInfo info;
        std::string manufacturer, product, serial;
    };

    // 和 Linux 内核的做法基本一致（drivers/usb/core/hub.c）。
    std::optional<Enumerated> enumerate(uint8_t new_address)
    {
        Enumerated e;
        log(kTransfer, "\n【枚举 1】复位总线，设备回到地址 0");
        reset_bus();

        log(kTransfer, "【枚举 2】在地址 0 上读设备描述符，只为知道端点 0 的最大包长");
        auto first = control_in(0, SetupPacket::get_descriptor(desc::DEVICE, 0, 64));
        if (!first || first->size() < 8) {
            log(kTransfer, "枚举失败：读不到设备描述符");
            return std::nullopt;
        }
        uint8_t mps = (*first)[7];
        log(kTransfer, "        收到 %zu 字节，bMaxPacketSize0 = %u", first->size(), mps);

        log(kTransfer, "【枚举 3】再复位一次（Linux 和 Windows 都这么做）");
        reset_bus();

        log(kTransfer, "【枚举 4】分配地址 %u", new_address);
        if (!control_out(0, SetupPacket::set_address(new_address))) {
            log(kTransfer, "枚举失败：SET_ADDRESS 没有完成");
            return std::nullopt;
        }
        mps0_[new_address] = mps;
        wait_ms(2);   // 规范给设备 2 ms 时间切换地址
        e.address = new_address;

        log(kTransfer, "【枚举 5】在新地址上读完整的设备描述符（18 字节）");
        auto dd_bytes = control_in(new_address, SetupPacket::get_descriptor(desc::DEVICE, 0, 18));
        auto dd = dd_bytes ? DeviceDescriptor::parse(*dd_bytes) : std::nullopt;
        if (!dd) {
            log(kTransfer, "枚举失败：设备描述符不完整");
            return std::nullopt;
        }
        e.device = *dd;

        log(kTransfer, "【枚举 6】先读配置描述符的前 9 字节，看看总长 wTotalLength 是多少");
        auto head = control_in(new_address,
                               SetupPacket::get_descriptor(desc::CONFIGURATION, 0, 9));
        if (!head || head->size() < 9) {
            log(kTransfer, "枚举失败：读不到配置描述符");
            return std::nullopt;
        }
        uint16_t total = get_u16(&(*head)[2]);
        log(kTransfer, "        wTotalLength = %u", total);

        log(kTransfer, "【枚举 7】再一次读出完整的配置描述符（%u 字节）", total);
        auto cfg = control_in(new_address,
                              SetupPacket::get_descriptor(desc::CONFIGURATION, 0, total));
        std::string why;
        auto info = cfg ? parse_config(*cfg, &why) : std::nullopt;
        if (!info) {
            log(kTransfer, "枚举失败：配置描述符不合法：%s", why.c_str());
            return std::nullopt;
        }
        e.config = *cfg;
        e.info = *info;

        log(kTransfer, "【枚举 8】读字符串描述符：语言列表、厂商、产品、序列号");
        auto langs = control_in(new_address, SetupPacket::get_descriptor(desc::STRING, 0, 255));
        uint16_t lang = (langs && langs->size() >= 4) ? get_u16(&(*langs)[2]) : 0x0409;
        auto read_string = [&](uint8_t index) -> std::string {
            if (index == 0) {
                return "";
            }
            auto s = control_in(new_address,
                                SetupPacket::get_descriptor(desc::STRING, index, 255, lang));
            return s ? string_from_descriptor(*s) : "";
        };
        e.manufacturer = read_string(e.device.iManufacturer);
        e.product = read_string(e.device.iProduct);
        e.serial = read_string(e.device.iSerialNumber);

        log(kTransfer, "【枚举 9】选中配置 %u（SET_CONFIGURATION），设备开始工作", info->value);
        if (!control_out(new_address, SetupPacket::set_configuration(info->value))) {
            log(kTransfer, "枚举失败：SET_CONFIGURATION 被拒绝");
            return std::nullopt;
        }
        return e;
    }

private:
    static uint16_t key(uint8_t addr, uint8_t ep_addr)
    {
        return static_cast<uint16_t>((addr << 8) | ep_addr);
    }

    uint16_t mps0(uint8_t addr) const
    {
        auto it = mps0_.find(addr);
        return it == mps0_.end() ? 64 : it->second;   // 还不知道时先假设 64
    }

    Result out_like(Pid token, uint8_t addr, uint8_t ep, bool toggle, const Bytes& data)
    {
        bus_.host_send(Packet::token(token, addr, ep));
        auto reply = bus_.host_send(Packet::data_packet(toggle, data));
        Result r = Result::Error;
        if (reply && reply->pid == Pid::Ack) {
            r = Result::Ack;
        } else if (reply && reply->pid == Pid::Nak) {
            r = Result::Nak;
        } else if (reply && reply->pid == Pid::Stall) {
            r = Result::Stall;
        }
        std::string what = std::string(pid_name(data_pid(toggle))) + " [" +
                           std::to_string(data.size()) + "] →";
        log_transaction(token == Pid::Setup ? "SETUP" : "OUT  ", addr, ep, what, r);
        return r;
    }

    // 出错（Error）就重试，最多 max_errors 次；NAK 就让设备先忙一会再试。
    template <typename F>
    Result retry(F transaction)
    {
        int errors = 0, naks = 0;
        for (;;) {
            Result r = transaction();
            if (r == Result::Error) {
                if (++errors >= max_errors) {
                    return r;
                }
                log(kTransaction, "  ↻ 第 %d 次出错，重试同一个事务", errors);
            } else if (r == Result::Nak) {
                if (++naks > max_naks) {
                    return r;
                }
                bus_.run_device_tasks();
            } else if (r != Result::Duplicate) {
                return r;
            }
        }
    }

    std::nullopt_t fail_control(const SetupPacket& s, const std::string& why)
    {
        flush_nak_log();
        log(kTransfer, "控制传输失败 %s：%s", describe(s).c_str(), why.c_str());
        return std::nullopt;
    }

    void log_transaction(const char* kind, uint8_t addr, uint8_t ep, const std::string& what,
                         std::optional<Result> r)
    {
        if (trace.level < kTransaction) {
            return;
        }
        // 连续的 NAK 只打印第一个，免得刷屏。
        if (r == Result::Nak) {
            if (nak_streak_++ > 0) {
                return;
            }
        } else {
            flush_nak_log();
        }
        char buf[200];
        std::snprintf(buf, sizeof buf, "%s addr=%u ep=%u  %s %s", kind, addr, ep, what.c_str(),
                      r ? result_name(*r) : "（同步传输，没有握手）");
        log(kTransaction, "%s", buf);
    }

    void flush_nak_log()
    {
        if (nak_streak_ > 1) {
            log(kTransaction, "  （省略了后面连续的 %d 个 NAK）", nak_streak_ - 1);
        }
        nak_streak_ = 0;
    }

    Bus& bus_;
    uint16_t frame_ = 0;
    int nak_streak_ = 0;
    std::map<uint16_t, bool> toggles_;   // 批量/中断端点的翻转位，键 = 地址<<8 | 端点地址
    std::map<uint8_t, uint8_t> mps0_;    // 每个地址的端点 0 最大包长
};

} // namespace musb
