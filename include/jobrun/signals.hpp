// ============================================================================
//  signals.hpp —— 用「自管道技巧」把信号安全地送进主循环
//
//  【知识点：信号处理器里什么都不能干】信号处理器可能在任意时刻打断主线程，
//  甚至打断 malloc 内部的临界区。所以处理器里只允许调用 async-signal-safe
//  的函数 —— 不能 printf、不能 iostream、不能 new、不能加锁。
//  能安全调用的东西少得可怜，write() 是其中之一。
//  于是标准做法是：处理器只 write 一个字节，主循环去读这根管道。
//  这就叫 self-pipe trick。
// ============================================================================
#pragma once

#include <string>

#include "jobrun/fd.hpp"

namespace jobrun {

/// 安装 SIGINT / SIGTERM / SIGHUP，并把它们变成「管道里可读的事件」。
class SignalWatcher {
public:
    SignalWatcher();
    ~SignalWatcher();

    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

    /// 等最多 timeout_ms 毫秒；期间收到过信号就返回信号编号，否则返回 0。
    int poll_once(int timeout_ms);

    [[nodiscard]] bool valid() const noexcept { return read_fd_.valid(); }

private:
    Fd read_fd_;
    Fd write_fd_;
};

/// 把信号编号翻成名字（SIGINT 之类），不认识的返回 "信号 N"。
std::string signal_name(int sig);

}  // namespace jobrun
