// libusb_raii.hpp —— 给 libusb 的 C 接口套一层 C++ 外壳：
// 资源在析构时自动释放，错误变成带说明的异常。
#pragma once

#include <libusb.h>

#include <stdexcept>
#include <string>

namespace usb {

// libusb 的错误码和第 4、7 章里学到的现象是一一对应的。
inline std::string explain(int err)
{
    switch (err) {
    case LIBUSB_ERROR_PIPE: return "LIBUSB_ERROR_PIPE：设备回了 STALL（请求不支持或端点被停用）";
    case LIBUSB_ERROR_TIMEOUT: return "LIBUSB_ERROR_TIMEOUT：超时（设备一直 NAK，或者缺了 ZLP）";
    case LIBUSB_ERROR_NO_DEVICE: return "LIBUSB_ERROR_NO_DEVICE：设备被拔掉了";
    case LIBUSB_ERROR_ACCESS: return "LIBUSB_ERROR_ACCESS：没有权限（见第 10 章 udev 规则）";
    case LIBUSB_ERROR_BUSY: return "LIBUSB_ERROR_BUSY：接口被别的驱动或程序占用了";
    case LIBUSB_ERROR_IO: return "LIBUSB_ERROR_IO：总线传输出错（连续 3 次出错）";
    case LIBUSB_ERROR_OVERFLOW: return "LIBUSB_ERROR_OVERFLOW：设备发来的数据比缓冲区大";
    default: return std::string(libusb_error_name(err)) + "：" + libusb_strerror(err);
    }
}

inline void check(int r, const char* what)
{
    if (r < 0) {
        throw std::runtime_error(std::string(what) + " 失败，" + explain(r));
    }
}

class Context {
public:
    Context() { check(libusb_init(&ctx_), "libusb_init"); }
    ~Context() { libusb_exit(ctx_); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    libusb_context* get() const { return ctx_; }

private:
    libusb_context* ctx_ = nullptr;
};

class DeviceList {
public:
    explicit DeviceList(const Context& ctx)
    {
        ssize_t n = libusb_get_device_list(ctx.get(), &list_);
        check(static_cast<int>(n), "libusb_get_device_list");
        count_ = static_cast<size_t>(n);
    }
    ~DeviceList() { libusb_free_device_list(list_, 1); }
    DeviceList(const DeviceList&) = delete;
    DeviceList& operator=(const DeviceList&) = delete;
    libusb_device** begin() const { return list_; }
    libusb_device** end() const { return list_ + count_; }

private:
    libusb_device** list_ = nullptr;
    size_t count_ = 0;
};

class Handle {
public:
    Handle() = default;
    explicit Handle(libusb_device_handle* h) : h_(h) {}
    ~Handle() { reset(); }
    Handle(Handle&& o) noexcept : h_(o.h_), claimed_(o.claimed_) { o.h_ = nullptr; o.claimed_ = -1; }
    Handle& operator=(Handle&& o) noexcept
    {
        if (this != &o) {
            reset();
            h_ = o.h_;
            claimed_ = o.claimed_;
            o.h_ = nullptr;
            o.claimed_ = -1;
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    explicit operator bool() const { return h_ != nullptr; }
    libusb_device_handle* get() const { return h_; }

    // 用批量/中断端点之前必须先“认领”接口，相当于告诉内核“这个接口归我管”。
    void claim(int itf)
    {
        libusb_set_auto_detach_kernel_driver(h_, 1);
        check(libusb_claim_interface(h_, itf), "libusb_claim_interface");
        claimed_ = itf;
    }

private:
    void reset()
    {
        if (h_) {
            if (claimed_ >= 0) {
                libusb_release_interface(h_, claimed_);
            }
            libusb_close(h_);
            h_ = nullptr;
        }
    }

    libusb_device_handle* h_ = nullptr;
    int claimed_ = -1;
};

inline const char* speed_name(int speed)
{
    switch (speed) {
    case LIBUSB_SPEED_LOW: return "低速 1.5M";
    case LIBUSB_SPEED_FULL: return "全速 12M";
    case LIBUSB_SPEED_HIGH: return "高速 480M";
    case LIBUSB_SPEED_SUPER: return "超速 5G";
    case LIBUSB_SPEED_SUPER_PLUS: return "超速+ 10G";
    default: return "未知速度";
    }
}

} // namespace usb
