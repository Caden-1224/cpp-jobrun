// ============================================================================
//  signals.hpp —— 用「自管道技巧」把信号安全地送进主循环
//
// ============================================================================
//  【给只会 C 的你：signal() 的三个坑】
//  ============================================================================
//
//  1. signal() 的行为在不同 Unix 上不一致（System V vs BSD 的语义不同），
//     POSIX 规定用 sigaction()，它把「处置函数、掩码、标志」一次说清。
//  2. 信号处理器可能在任何时刻打断主线程 —— 包括打断 malloc 内部的临界区、
//     std::string 的扩容、甚至 iostream 的锁。所以处理器里只允许调用
//     async-signal-safe 的函数，printf / new / iostream / 加锁全都不行。
//  3. 信号是「异步事件」，它不该直接改业务状态（你不知道它打断了什么），
//     而应该被转成「主循环能 select/poll 到的一件事」。
//
//  自管道技巧（self-pipe trick）就是第 3 点的标准答案：
//      处理器里只 write(fd, &sig, 1);
//      主循环 poll 这根管道，读到信号编号再处理。
//  write 是 async-signal-safe 的少数函数之一，而且写端设为非阻塞，
//  管道满时也只是 EAGAIN，绝不会在处理器里睡过去。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. 成员是「有析构函数的对象」
//         Fd read_fd_;
//         Fd write_fd_;
//     SignalWatcher 析构时，两个 Fd 自动 close，不需要手写 close。
//     这就是 RAII 的组合性：小资源管好自己，大对象就自动管好了。
//
//  2. 删除拷贝
//     一个 SignalWatcher 对应「一份已安装的信号处置 + 一对管道」，
//     拷贝它会得到两个对象抢同一根管道。所以拷贝删除、移动也不提供
//     （信号处置是进程级全局状态，移动同样会制造两份所有权）。
//
//  3. 返回 int 的轮询函数
//         int poll_once(int timeout_ms);
//     返回 0 表示「这次没收到信号」，非 0 是信号编号。
//     【C 对照】C 里通常返回 -1 表示错误、0 表示没信号；这里 0 是合法值，
//     因为信号编号不可能为 0。
//
// ============================================================================
//  【本文件涉及的 POSIX API】
// ============================================================================
//
//  int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
//      signum : 信号编号（SIGINT / SIGTERM / SIGHUP ...）
//      act    : 新处置；NULL 表示只查询
//      oldact : 旧处置；NULL 表示不关心
//      返回   : 0 成功，-1 + errno
//      struct sigaction 的关键字段：
//          sa_handler  简单处理器 void (*)(int)
//          sa_sigaction 带 siginfo 的处理器（需要 SA_SIGINFO）
//          sa_mask     处理器执行期间额外屏蔽哪些信号
//          sa_flags    SA_RESTART / SA_NODEFER / SA_RESETHAND / SA_SIGINFO ...
//      【坑】sa_mask 必须用 sigemptyset 初始化；直接 memset 0 不保证正确。
//
//  int poll(struct pollfd *fds, nfds_t nfds, int timeout);   （见 process.hpp）
//
//  【扩展】
//      signalfd(2)  ：把信号变成一个可以直接 poll 的 fd（Linux 专有，
//                     比自管道更干净，但需要先 sigprocmask 屏蔽信号）
//      sigwait(3)   ：同步地「等」信号，适合专门的信号线程
//      sigprocmask(2)：屏蔽/解除屏蔽信号，常和 signalfd/sigwait 配合
//      ppoll(2)     ：poll + 原子地替换信号掩码，避免「检查信号 → 进入 poll」
//                     之间的竞态窗口
// ============================================================================
#pragma once

#include <string>

#include "jobrun/fd.hpp"

namespace jobrun {

/// 安装 SIGINT / SIGTERM / SIGHUP，并把它们变成「管道里可读的事件」。
///
/// 【生命周期】构造 = 建管道 + 安装处理器；析构 = 恢复默认处置 + 关管道。
///             【坑】析构顺序很重要：必须先恢复处置，再让 Fd 关闭。
///             反过来的话，处理器可能 write 到一个已经关闭（甚至被复用）的 fd。
class SignalWatcher {
public:
    SignalWatcher();
    ~SignalWatcher();

    /// 【语法】= delete 的拷贝构造/赋值让这个类完全不可复制。
    ///         【扩展】如果确实需要移动，要实现「移动后把源对象的全局 fd 置空」，
    ///         否则两个对象会同时认为自己是信号接收者。
    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

    /// 等最多 timeout_ms 毫秒；期间收到过信号就返回信号编号，否则返回 0。
    ///
    /// 【参数】timeout_ms：传给 poll() 的超时。100 表示最多阻塞 100ms，
    ///         这样调用方可以周期性检查「是否该退出」。
    /// 【返回】信号编号（> 0）或 0（超时/没收到）。
    int poll_once(int timeout_ms);

    [[nodiscard]] bool valid() const noexcept { return read_fd_.valid(); }

private:
    Fd read_fd_;
    Fd write_fd_;
};

/// 把信号编号翻成名字（SIGINT 之类），不认识的返回 "信号 N"。
///
/// 【语法】返回 std::string 而不是 const char*：
///         未识别的信号需要动态拼接 "信号 34"，返回 string 才能统一。
///         代价是一次分配；这个函数只在收到信号时调用，不在热路径上。
std::string signal_name(int sig);

}  // namespace jobrun
