// device.hpp —— 设备端协议栈（第 4、7 章）。真实 MCU 上这部分由 USB 控制器
// 硬件 + TinyUSB 之类的协议栈完成，这里用几百行 C++ 把它们的逻辑摊开给你看。
//
// 职责分三层：
//   1. 包层：看令牌包里的地址是不是自己，决定回答 DATA / ACK / NAK / STALL
//   2. 端点层：每个端点一个发送队列、一个接收缓冲、一个数据翻转位（DATA0/1）
//   3. 控制传输：端点 0 上的“建立-数据-状态”三阶段，以及第 9 章的标准请求
//
// 具体的设备（键盘、串口……）继承 Device，通过几个虚函数处理类请求。
#pragma once

#include <algorithm>
#include <deque>
#include <map>
#include <string>

#include "bus.hpp"
#include "descriptors.hpp"
#include "setup.hpp"

namespace musb {

class Device : public BusDevice {
public:
    // USB 2.0 规范图 9-1 的设备状态（省略了 Attached 和 Suspended）
    enum class State { Powered, Default, Address, Configured };

    Device(std::string name, const DeviceDescriptor& dd, Bytes config,
           std::vector<std::string> strings)
        : name_(std::move(name)), dd_(dd), config_(std::move(config)),
          strings_(std::move(strings))
    {
        reset_endpoints();
    }

    const char* name() const override { return name_.c_str(); }
    State state() const { return state_; }
    uint8_t address() const { return address_; }

    // ---- 固件（应用）这一侧用的接口 -------------------------------------
    // 把一段数据放进 IN 端点的发送队列，按最大包长切成若干包。
    // zlp = true 时，如果长度正好是最大包长的整数倍，再补一个零长度包，
    // 让主机知道“这次传输结束了”（第 9 章会看到不补的后果）。
    void write(uint8_t ep_addr, const Bytes& data, bool zlp = true)
    {
        if (Endpoint* e = find(ep_addr)) {
            queue_packets(*e, data, zlp);
        }
    }
    bool tx_busy(uint8_t ep_addr)
    {
        Endpoint* e = find(ep_addr);
        return e && !e->tx.empty();
    }
    // 取走 OUT 端点收到的全部数据。
    Bytes read(uint8_t ep_addr)
    {
        Bytes out;
        if (Endpoint* e = find(ep_addr)) {
            out.swap(e->rx);
        }
        return out;
    }
    size_t available(uint8_t ep_addr)
    {
        Endpoint* e = find(ep_addr);
        return e ? e->rx.size() : 0;
    }
    void set_stall(uint8_t ep_addr, bool stall)
    {
        if (Endpoint* e = find(ep_addr)) {
            e->stalled = stall;
        }
    }
    // OUT 端点的接收缓冲大小。缓冲满了设备就回 NAK，这就是 USB 的流量控制。
    void set_rx_capacity(uint8_t ep_addr, size_t bytes)
    {
        if (Endpoint* e = find(ep_addr)) {
            e->rx_capacity = bytes;
        }
    }
    // 只给第 4 章用：跳过枚举，直接把设备变成“已配置”。
    void debug_force_configured(uint8_t addr)
    {
        address_ = addr;
        apply_configuration(config_[5]);
    }

    // 故意做错的地方，用来观察主机的反应（第 7 章）。
    bool bug_set_address_too_early = false;

    // ---- 总线这一侧 --------------------------------------------------------
    void on_bus_reset() override
    {
        address_ = 0;
        pending_address_ = 0;
        state_ = State::Default;
        reset_endpoints();
        log(kTransaction, "[%s] 复位：地址 = 0，进入 Default 状态", name());
    }

    std::optional<Packet> on_packet(const Packet& p) override
    {
        if (state_ == State::Powered) {
            return std::nullopt;   // 还没被复位过，不理会任何包
        }
        switch (p.kind()) {
        case PidKind::Token:
            return on_token(p);
        case PidKind::Data:
            return on_data(p);
        case PidKind::Handshake:
            on_handshake(p);
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

protected:
    // ---- 给具体设备类覆写的钩子 -------------------------------------------
    // 返回 false = 不支持，协议栈会回 STALL。
    // IN 请求：把要回给主机的数据放进 data；OUT 请求：data 里是主机发来的数据。
    virtual bool on_class_request(const SetupPacket&, Bytes& /*data*/) { return false; }
    virtual bool on_vendor_request(const SetupPacket&, Bytes& /*data*/) { return false; }
    // GET_DESCRIPTOR 请求的类型不是设备/配置/字符串时调用（如 HID 报告描述符）。
    virtual bool get_other_descriptor(const SetupPacket&, Bytes& /*out*/) { return false; }
    virtual void on_configured(uint8_t /*value*/) {}

    struct Endpoint {
        uint8_t address = 0;
        TransferType type = TransferType::Control;
        uint16_t max_packet = 64;
        bool toggle = false;          // 下一个要发 / 期望收到的是 DATA0 还是 DATA1
        bool stalled = false;
        std::deque<Bytes> tx;         // IN：排队等主机来取的包
        Bytes rx;                     // OUT：已经收到的数据
        size_t rx_capacity = 4096;
        bool full_reported = false;   // 只在第一次 NAK 时打印，免得刷屏
    };

    Endpoint* find(uint8_t ep_addr)
    {
        auto it = eps_.find(ep_addr);
        return it == eps_.end() ? nullptr : &it->second;
    }

private:
    enum class Ctrl { Idle, DataIn, DataOut, StatusIn };

    // ---- 包层 ---------------------------------------------------------------
    std::optional<Packet> on_token(const Packet& p)
    {
        if (pending_in_) {
            // 上次发出的数据没等到 ACK（主机没收到，或 ACK 被打坏了）。
            // 数据留在队列里，翻转位不变，下次 IN 原样重发。
            log(kTransaction, "[%s] 没等到 ACK，那包数据留着下次重发", name());
            pending_in_ = nullptr;
        }
        token_ = std::nullopt;
        if (p.addr != address_) {
            return std::nullopt;   // 不是叫我
        }
        if (p.pid == Pid::In) {
            return on_in(p.endp);
        }
        if (p.pid == Pid::Out || p.pid == Pid::Setup) {
            token_ = p;            // 等后面的数据包
        }
        return std::nullopt;
    }

    std::optional<Packet> on_data(const Packet& p)
    {
        if (!token_) {
            return std::nullopt;
        }
        Packet tok = *token_;
        token_ = std::nullopt;
        if (tok.pid == Pid::Setup) {
            return on_setup(p);
        }
        return on_out(tok.endp, p);
    }

    void on_handshake(const Packet& p)
    {
        if (!pending_in_ || p.pid != Pid::Ack) {
            return;
        }
        // 主机确认收到了：这一包才算真正发完，翻转位前进。
        Endpoint* e = pending_in_;
        pending_in_ = nullptr;
        e->tx.pop_front();
        e->toggle = !e->toggle;
        if (e->address == 0x80 && ctrl_ == Ctrl::StatusIn && e->tx.empty()) {
            finish_control();
        }
    }

    // ---- 端点层 -------------------------------------------------------------
    std::optional<Packet> on_in(uint8_t endp)
    {
        Endpoint* e = find(static_cast<uint8_t>(0x80 | endp));
        if (!e) {
            return std::nullopt;   // 没有这个端点（或者还没配置），不回答
        }
        if (e->stalled) {
            return Packet::handshake(Pid::Stall);
        }
        if (e->type == TransferType::Isochronous) {
            // 同步传输：没有握手、没有重传、没有数据翻转，永远 DATA0。
            Bytes d;
            if (!e->tx.empty()) {
                d = e->tx.front();
                e->tx.pop_front();
            }
            return Packet::data_packet(false, d);
        }
        if (e->tx.empty()) {
            return Packet::handshake(Pid::Nak);   // 现在没东西给你，待会再来
        }
        pending_in_ = e;
        return Packet::data_packet(e->toggle, e->tx.front());
    }

    std::optional<Packet> on_out(uint8_t endp, const Packet& p)
    {
        Endpoint* e = find(endp);
        if (!e) {
            return std::nullopt;
        }
        if (e->type == TransferType::Isochronous) {
            e->rx.insert(e->rx.end(), p.data.begin(), p.data.end());
            return std::nullopt;   // 同步传输不回握手包
        }
        if (e->stalled) {
            return Packet::handshake(Pid::Stall);
        }
        bool toggle = (p.pid == Pid::Data1);
        if (toggle != e->toggle) {
            // 翻转位和期望的不一样：说明这是主机重发的、我们已经收过的包
            // （上次我们回的 ACK 主机没收到）。回 ACK 让主机放心，但数据丢掉。
            log(kTransaction, "[%s] 收到 %s，期望 %s：这是重发的旧包，回 ACK 但丢弃数据", name(),
                pid_name(p.pid), pid_name(data_pid(e->toggle)));
            return Packet::handshake(Pid::Ack);
        }
        if (endp == 0) {
            return on_ep0_out(*e, p);
        }
        if (e->rx.size() + p.data.size() > e->rx_capacity) {
            if (!e->full_reported) {
                log(kTransaction, "[%s] 端点 %u 的接收缓冲满了，回 NAK", name(), endp);
                e->full_reported = true;
            }
            return Packet::handshake(Pid::Nak);
        }
        e->full_reported = false;
        e->rx.insert(e->rx.end(), p.data.begin(), p.data.end());
        e->toggle = !e->toggle;
        return Packet::handshake(Pid::Ack);
    }

    static void queue_packets(Endpoint& e, const Bytes& data, bool zlp)
    {
        for (size_t i = 0; i < data.size(); i += e.max_packet) {
            size_t n = std::min<size_t>(e.max_packet, data.size() - i);
            e.tx.emplace_back(data.begin() + static_cast<long>(i),
                              data.begin() + static_cast<long>(i + n));
        }
        if (zlp && data.size() % e.max_packet == 0) {
            e.tx.emplace_back();   // 零长度包（ZLP）
        }
    }

    void reset_endpoints()
    {
        eps_.clear();
        Endpoint out0, in0;
        out0.address = 0x00;
        in0.address = 0x80;
        out0.max_packet = in0.max_packet = dd_.bMaxPacketSize0;
        eps_[0x00] = out0;
        eps_[0x80] = in0;
        ctrl_ = Ctrl::Idle;
        pending_in_ = nullptr;
        token_ = std::nullopt;
    }

    void apply_configuration(uint8_t value)
    {
        Endpoint out0 = eps_[0x00], in0 = eps_[0x80];
        eps_.clear();
        eps_[0x00] = out0;
        eps_[0x80] = in0;
        if (auto info = parse_config(config_)) {
            for (const InterfaceInfo& itf : info->interfaces) {
                if (itf.alt != 0) {
                    continue;
                }
                for (const EndpointInfo& ep : itf.endpoints) {
                    Endpoint e;
                    e.address = ep.address;
                    e.type = ep.type;
                    e.max_packet = ep.max_packet;
                    eps_[ep.address] = e;
                }
            }
        }
        config_value_ = value;
        state_ = State::Configured;
        log(kTransaction, "[%s] 进入 Configured 状态：端点已按配置描述符建立，可以干活了", name());
        on_configured(value);
    }

    // ---- 控制传输（端点 0） --------------------------------------------------
    std::optional<Packet> on_setup(const Packet& p)
    {
        auto s = SetupPacket::from_bytes(p.data);
        if (p.pid != Pid::Data0 || !s) {
            return std::nullopt;   // SETUP 后面必须是 8 字节的 DATA0
        }
        // SETUP 永远要回 ACK，并且会打断上一个没做完的控制传输、解除端点 0 的 STALL。
        Endpoint& out0 = eps_[0x00];
        Endpoint& in0 = eps_[0x80];
        out0.stalled = in0.stalled = false;
        in0.tx.clear();
        out0.toggle = in0.toggle = true;   // 数据阶段和状态阶段都从 DATA1 开始
        setup_ = *s;
        ctrl_data_.clear();
        log(kTransaction, "[%s] 收到请求 %s", name(), describe(*s).c_str());

        if (s->is_in() && s->wLength > 0) {
            Bytes reply;
            if (dispatch(*s, reply)) {
                if (reply.size() > s->wLength) {
                    reply.resize(s->wLength);   // 主机要多少就最多给多少
                }
                queue_packets(in0, reply, reply.size() < s->wLength);
                ctrl_ = Ctrl::DataIn;
            } else {
                stall_ep0();
            }
        } else if (!s->is_in() && s->wLength > 0) {
            ctrl_ = Ctrl::DataOut;   // 先收数据，收齐了再处理
        } else {
            Bytes none;
            if (dispatch(*s, none)) {
                queue_packets(in0, {}, true);   // 状态阶段：回一个零长度 DATA1
                ctrl_ = Ctrl::StatusIn;
            } else {
                stall_ep0();
            }
        }
        return Packet::handshake(Pid::Ack);
    }

    std::optional<Packet> on_ep0_out(Endpoint& out0, const Packet& p)
    {
        if (ctrl_ == Ctrl::DataOut) {
            ctrl_data_.insert(ctrl_data_.end(), p.data.begin(), p.data.end());
            out0.toggle = !out0.toggle;
            if (ctrl_data_.size() >= setup_.wLength) {
                if (dispatch(setup_, ctrl_data_)) {
                    queue_packets(eps_[0x80], {}, true);
                    ctrl_ = Ctrl::StatusIn;
                } else {
                    stall_ep0();   // 状态阶段主机会收到 STALL
                }
            }
            return Packet::handshake(Pid::Ack);
        }
        if (ctrl_ == Ctrl::DataIn) {
            // 主机发来零长度 OUT：这是控制读的状态阶段。主机可能没把数据
            // 读完就进入状态阶段了（比如只读了前 8 字节），剩下的丢掉即可。
            eps_[0x80].tx.clear();
            out0.toggle = !out0.toggle;
            finish_control();
            return Packet::handshake(Pid::Ack);
        }
        return Packet::handshake(Pid::Stall);
    }

    void stall_ep0()
    {
        log(kTransaction, "[%s] 不支持这个请求，端点 0 回 STALL", name());
        eps_[0x00].stalled = eps_[0x80].stalled = true;
        ctrl_ = Ctrl::Idle;
    }

    // 状态阶段完成，整个控制传输才算结束。
    void finish_control()
    {
        ctrl_ = Ctrl::Idle;
        if (setup_.type() == req::kTypeStandard && setup_.bRequest == req::SET_ADDRESS &&
            !bug_set_address_too_early) {
            set_address_now(pending_address_);
        }
    }

    void set_address_now(uint8_t addr)
    {
        address_ = addr;
        state_ = addr ? State::Address : State::Default;
        log(kTransaction, "[%s] 地址改为 %u，进入 %s 状态", name(), addr,
            addr ? "Address" : "Default");
    }

    bool dispatch(const SetupPacket& s, Bytes& data)
    {
        switch (s.type()) {
        case req::kTypeStandard: return standard_request(s, data);
        case req::kTypeClass: return on_class_request(s, data);
        case req::kTypeVendor: return on_vendor_request(s, data);
        default: return false;
        }
    }

    // USB 2.0 规范第 9.4 节的标准请求，每个 USB 设备都必须支持。
    bool standard_request(const SetupPacket& s, Bytes& data)
    {
        switch (s.bRequest) {
        case req::GET_DESCRIPTOR: {
            uint8_t type = static_cast<uint8_t>(s.wValue >> 8);
            uint8_t index = static_cast<uint8_t>(s.wValue & 0xFF);
            if (type == desc::DEVICE) {
                data = dd_.to_bytes();
                return true;
            }
            if (type == desc::CONFIGURATION && index == 0) {
                data = config_;
                return true;
            }
            if (type == desc::STRING) {
                if (index == 0) {
                    data = langid_descriptor();
                    return true;
                }
                if (index <= strings_.size()) {
                    data = string_descriptor(strings_[index - 1]);
                    return true;
                }
                return false;
            }
            // 只支持全速的设备，对 DEVICE_QUALIFIER 必须回 STALL。
            return get_other_descriptor(s, data);
        }
        case req::SET_ADDRESS:
            pending_address_ = static_cast<uint8_t>(s.wValue & 0x7F);
            if (bug_set_address_too_early) {
                set_address_now(pending_address_);   // 错误示范：状态阶段还没完成
            }
            return true;
        case req::GET_CONFIGURATION:
            data = {state_ == State::Configured ? config_value_ : uint8_t{0}};
            return true;
        case req::SET_CONFIGURATION:
            if (state_ == State::Default) {
                return false;
            }
            if (s.wValue == 0) {
                state_ = State::Address;
                config_value_ = 0;
                return true;
            }
            if (s.wValue != config_[5]) {
                return false;
            }
            apply_configuration(static_cast<uint8_t>(s.wValue));
            return true;
        case req::GET_STATUS:
            if (s.recipient() == req::kRecipEndpoint) {
                Endpoint* e = find(static_cast<uint8_t>(s.wIndex));
                if (!e) {
                    return false;
                }
                data = {static_cast<uint8_t>(e->stalled ? 1 : 0), 0};
            } else {
                // 设备：bit0 = 自供电，bit1 = 远程唤醒已开启
                data = {static_cast<uint8_t>((config_[7] & 0x40) ? 1 : 0), 0};
            }
            return true;
        case req::CLEAR_FEATURE:
        case req::SET_FEATURE:
            if (s.recipient() == req::kRecipEndpoint && s.wValue == req::FEATURE_ENDPOINT_HALT) {
                Endpoint* e = find(static_cast<uint8_t>(s.wIndex));
                if (!e) {
                    return false;
                }
                e->stalled = (s.bRequest == req::SET_FEATURE);
                if (!e->stalled) {
                    e->toggle = false;   // 清除 HALT 时数据翻转位必须回到 DATA0
                    log(kTransaction, "[%s] 端点 0x%02X 解除 STALL，翻转位回到 DATA0", name(),
                        e->address);
                }
                return true;
            }
            return false;
        case req::GET_INTERFACE:
            data = {0};
            return true;
        case req::SET_INTERFACE:
            return s.wValue == 0;
        default:
            return false;
        }
    }

    std::string name_;
    DeviceDescriptor dd_;
    Bytes config_;
    std::vector<std::string> strings_;   // 下标 i 对应字符串描述符 i+1

    State state_ = State::Powered;
    uint8_t address_ = 0;
    uint8_t pending_address_ = 0;
    uint8_t config_value_ = 0;
    std::map<uint8_t, Endpoint> eps_;    // 键是端点地址：0x00 = EP0 OUT，0x80 = EP0 IN
    std::optional<Packet> token_;        // 刚收到、正等着数据包的 OUT/SETUP 令牌
    Endpoint* pending_in_ = nullptr;     // 已发出数据、正等着主机 ACK 的端点

    Ctrl ctrl_ = Ctrl::Idle;
    SetupPacket setup_;
    Bytes ctrl_data_;
};

} // namespace musb
