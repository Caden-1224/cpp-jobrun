// ============================================================================
//  process.hpp —— 真实子进程：创建、收割、以及把它的输出抓回来
//
//  这个模块是整份练习里最「操作系统」的部分：
//  fork / exec / pipe / dup2 / poll / waitpid / kill / 进程组 / 信号，
//  全部是真实系统调用，不是模拟。
// ============================================================================
#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "jobrun/fd.hpp"

namespace jobrun {

/// 一个作业最终走到了哪一步。
enum class RunState {
    Succeeded,   ///< 正常退出，退出码 0
    Failed,      ///< 正常退出，但退出码非 0
    Signaled,    ///< 被信号终止（段错误、被 kill 等）
    TimedOut,    ///< 超过时限，被我们 SIGKILL 掉
    Cancelled,   ///< 用户按下 Ctrl-C，主动收工
    SpawnError,  ///< fork 或 exec 失败
};

/// 把 RunState 印成人看的字。
const char* to_string(RunState state) noexcept;

/// waitpid 交回来的状态，拆成「怎么结束的」。
struct ExitStatus {
    bool reaped = false;
    bool exited = false;   ///< WIFEXITED
    bool signaled = false; ///< WIFSIGNALED
    int code = 0;          ///< 正常退出码
    int term_signal = 0;   ///< 终止它的信号号
    std::string describe() const;
};

/// 子进程句柄（RAII）。
///
/// 【知识点：为什么需要一个类】裸 pid_t 只是一个 int。谁负责回收它？
/// 忘了 waitpid 就留下僵尸进程（zombie）—— 进程已经死了，但内核里的
/// task_struct 还占着，因为退出码没人取走。把这个责任写进析构函数，
/// 「忘记回收」这件事就变得不可能发生。
class ChildProcess {
public:
    ChildProcess() noexcept = default;
    explicit ChildProcess(pid_t pid) noexcept;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    [[nodiscard]] bool valid() const noexcept { return pid_ > 0; }
    [[nodiscard]] bool reaped() const noexcept { return reaped_; }

    /// 向「整个进程组」发信号。
    ///
    /// 【知识点：为什么是 -pid 而不是 pid】子进程在启动时自成一个进程组，
    /// 组长就是它自己。kill(-pid) 会把信号发给组里所有成员 —— 包括
    /// 它 fork 出来的孙子进程。只杀直接子进程的话，
    /// `sh -c 'sleep 100'` 里的 sleep 会活下来变成孤儿，
    /// 继续占着它继承到的管道写端，于是父进程那边永远等不到 EOF。
    void signal_group(int sig) const;

    /// 非阻塞回收。返回 nullopt 表示孩子还在跑。
    std::optional<ExitStatus> try_reap() noexcept;

    /// 阻塞回收。
    ExitStatus reap() noexcept;

private:
    pid_t pid_ = -1;
    bool reaped_ = false;
};

/// 抓取选项。
struct CaptureOptions {
    int timeout_ms = 5000;
    std::size_t max_preview_lines = 4;
    /// 停止标志。用户按 Ctrl-C 时被置位，正在跑的作业会被收掉。
    const std::atomic<bool>* stop = nullptr;
};

/// 一次命令执行的完整结果。
struct CaptureResult {
    RunState state = RunState::SpawnError;
    ExitStatus status;
    long long duration_us = 0;   ///< 从 fork 到回收，微秒
    std::size_t out_lines = 0;
    std::size_t err_lines = 0;
    std::size_t out_bytes = 0;
    std::size_t err_bytes = 0;
    bool truncated = false;      ///< 是否有某一行太长被截断过
    int spawn_errno = 0;         ///< 仅 SpawnError 时有效：exec 失败时的 errno
    std::vector<std::string> out_preview;
    std::vector<std::string> err_preview;
};

/// fork + exec 一条命令，把 stdout/stderr 全部读回来，并完成回收。
///
/// 这个函数在做四件事，缺一不可：
///   1. 建两根管道，fork，子进程里 dup2 重定向后 exec；
///   2. 父进程立刻关掉两个写端（否则 read 永远等不到 EOF）；
///   3. 用 poll + 非阻塞 read 持续把管道抽干
///      （不抽的话，子进程写满 64KiB 管道缓冲区就会永久阻塞）；
///   4. 超时或取消时，向整个进程组发 SIGKILL，再 waitpid 回收。
///
/// 失败以异常抛出（fork 失败、poll 失败）；子进程自己失败不算异常，
/// 而是通过 CaptureResult::state 返回 —— 这是「错误」与「异常」的分界：
/// 作业失败是正常业务，系统调用失败才是异常。
CaptureResult run_command_captured(const std::vector<std::string>& argv, const CaptureOptions& options);

}  // namespace jobrun
