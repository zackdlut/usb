// bus.hpp —— 一条模拟的 USB 总线（第 4 章起使用）。
//
// 真实的 USB 是“主机说、设备答”：主机发出的每个包，总线上所有设备都能
// 听到，只有被点名（地址匹配）的设备才会回答。这里用一个 Bus 对象模拟：
//
//   主机 --host_send(包)--> 编码成字节 --(可能被噪声打坏)--> 每个设备 on_packet()
//   主机 <----------------- 解码 <--(可能被噪声打坏)-- 被点名的设备的回答
//
// 为了简单，多个设备直接挂在同一条总线上；真实世界里它们之间隔着 Hub。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "packet.hpp"
#include "signal.hpp"
#include "trace.hpp"

namespace musb {

enum class Dir { HostToDevice, DeviceToHost };

// 总线上的设备需要实现的接口。
class BusDevice {
public:
    virtual ~BusDevice() = default;
    virtual const char* name() const = 0;
    // 主机发出的每个包都会调用它。返回值是设备的回答（握手包或数据包），
    // 不回答就返回 nullopt —— 主机那边会表现为“超时”。
    virtual std::optional<Packet> on_packet(const Packet& p) = 0;
    virtual void on_bus_reset() {}
    virtual void on_sof(uint16_t /*frame*/) {}
    // 设备固件主循环里“除了 USB 以外的事情”，比如准备要发的数据。
    virtual void task() {}
};

class Bus {
public:
    void attach(BusDevice& d)
    {
        devices_.push_back(&d);
        log(kTransfer, "[总线] 插入设备 \"%s\"：设备把 D+ 上拉到 3.3V，主机检测到一个全速设备",
            d.name());
    }

    void detach(BusDevice& d)
    {
        for (size_t i = 0; i < devices_.size(); i++) {
            if (devices_[i] == &d) {
                devices_.erase(devices_.begin() + static_cast<long>(i));
                log(kTransfer, "[总线] 拔出设备 \"%s\"", d.name());
                return;
            }
        }
    }

    // 主机把 D+、D- 同时拉低（SE0）至少 10 ms：所有设备回到地址 0。
    void reset()
    {
        log(kTransaction, "[总线] 复位：SE0 保持 10 ms，所有设备回到“默认状态”，地址 = 0");
        for (BusDevice* d : devices_) {
            d->on_bus_reset();
        }
    }

    // 每 1 ms 一个 SOF（Start Of Frame），广播帧号，没有人回答。
    void sof(uint16_t frame)
    {
        Packet p = Packet::sof(frame);
        if (trace.show_sof) {
            deliver(Dir::HostToDevice, p);
        }
        for (BusDevice* d : devices_) {
            d->on_sof(p.frame);
        }
    }

    void run_device_tasks()
    {
        for (BusDevice* d : devices_) {
            d->task();
        }
    }

    // 主机发一个包。返回主机收到的回答（已经通过了 CRC 检查），没有回答或
    // 回答被打坏了就返回 nullopt。
    std::optional<Packet> host_send(const Packet& p)
    {
        std::optional<Packet> seen = deliver(Dir::HostToDevice, p);
        if (!seen) {
            return std::nullopt;   // 设备收到的是坏包，按规范直接装没听见
        }
        std::optional<Packet> reply;
        for (BusDevice* d : devices_) {
            std::optional<Packet> r = d->on_packet(*seen);
            if (r) {
                if (reply) {
                    log(kTransfer, "[总线] 错误：两个设备同时回答（地址冲突）");
                }
                reply = std::move(r);
            }
        }
        if (!reply) {
            return std::nullopt;
        }
        return deliver(Dir::DeviceToHost, *reply);
    }

    // 故障注入：让下一个（跳过 skip 个之后的）方向为 dir、PID 为 pid 的包
    // 在线上被打坏一个比特，用来观察 USB 怎样从错误中恢复。
    void corrupt_next(Dir dir, Pid pid, int skip = 0)
    {
        faults_.push_back({dir, pid, skip});
    }

private:
    struct Fault {
        Dir dir;
        Pid pid;
        int skip;
    };

    std::optional<Packet> deliver(Dir dir, const Packet& p)
    {
        Bytes wire = encode(p);
        const char* arrow = dir == Dir::HostToDevice ? "主机→设备" : "设备→主机";
        char line[160];
        std::snprintf(line, sizeof line, "%s  %-34s | %s", arrow, describe(p).c_str(),
                      hex(wire, 12).c_str());
        log(kPacket, "%s", line);
        if (trace.level >= kWire) {
            log(kWire, "线路: %s", to_string(to_wire(wire)).c_str());
        }

        for (size_t i = 0; i < faults_.size(); i++) {
            Fault& f = faults_[i];
            if (f.dir == dir && f.pid == p.pid) {
                if (f.skip > 0) {
                    f.skip--;
                    continue;
                }
                faults_.erase(faults_.begin() + static_cast<long>(i));
                wire.back() ^= 0x01;
                log(kTransaction, "⚡ 噪声打坏了 %s 包的一个比特", pid_name(p.pid));
                break;
            }
        }

        Decoded d = decode(wire);
        if (!d.packet) {
            log(kTransaction, "✗ %s丢弃了这个包：%s",
                dir == Dir::HostToDevice ? "设备" : "主机", d.error.c_str());
        }
        return d.packet;
    }

    std::vector<BusDevice*> devices_;
    std::vector<Fault> faults_;
};

} // namespace musb
