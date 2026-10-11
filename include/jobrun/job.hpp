// ============================================================================
//  job.hpp —— 作业的抽象接口，以及两种具体的作业
//
//  【知识点：继承与多态】Job 只描述「一个作业能做什么」，
//  至于「怎么执行」，交给子类。ShellJob 真的 fork 一个进程，
//  BuiltinJob 就在当前线程里跑完 —— 对调用方来说两者没有区别，
//  这正是多态存在的理由：把「变化的部分」关进子类。
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "jobrun/process.hpp"

namespace jobrun {

/// 一个作业跑完之后留下的一切。
struct JobOutcome {
    RunState state = RunState::SpawnError;
    std::string detail;          ///< 一句话交代怎么结束的
    long long duration_us = 0;
    std::size_t out_lines = 0;
    std::size_t err_lines = 0;
    std::size_t out_bytes = 0;
    std::size_t err_bytes = 0;
    bool truncated = false;
    std::vector<std::string> out_preview;
    std::vector<std::string> err_preview;
};

/// 执行一个作业时外部给它的约束。
struct JobContext {
    int timeout_ms = 5000;
    std::size_t max_preview_lines = 3;
    /// 指向全局停止标志。用户按 Ctrl-C 时它会变成 true。
    /// 【知识点：为什么用指针】这里只需要「看」它，不需要「拥有」它；
    /// 而且 std::atomic<bool> 不可拷贝，用引用或指针是唯一自然的选择。
    const std::atomic<bool>* stop = nullptr;
};

/// 作业的抽象基类。
class Job {
public:
    // 【知识点：虚析构函数】通过 Job* 删除一个 ShellJob 时，
    // 如果析构函数不是虚的，只会调用 Job 的析构 —— 子类成员泄漏。
    // 一条铁律：只要一个类打算被继承并通过基类指针删除，析构就必须是虚的。
    virtual ~Job() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string command_line() const = 0;
    [[nodiscard]] virtual JobOutcome run(const JobContext& context) const = 0;
    /// 这个作业会不会真的开出子进程？用来在报告里区分两类作业。
    [[nodiscard]] virtual bool uses_process() const noexcept = 0;
};

/// 跑一条外部命令：fork + exec + 抓输出。
class ShellJob final : public Job {
public:
    ShellJob(std::string name, std::vector<std::string> argv);

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string command_line() const override;
    [[nodiscard]] JobOutcome run(const JobContext& context) const override;
    [[nodiscard]] bool uses_process() const noexcept override { return true; }

private:
    std::string name_;
    std::vector<std::string> argv_;
};

/// 在当前线程里直接跑一段逻辑，完全不开进程。
///
/// 现实里这类作业很常见：一次内部一致性检查、一次汇总计算，
/// 为它 fork 一个进程纯属浪费。把它做成 Job 的另一个子类，
/// 上层的线程池和报告逻辑一行都不用改。
class BuiltinJob final : public Job {
public:
    /// 返回它「打印」出来的若干行；抛异常即视为作业失败。
    using Body = std::function<std::vector<std::string>()>;

    BuiltinJob(std::string name, Body body);

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string command_line() const override;
    [[nodiscard]] JobOutcome run(const JobContext& context) const override;
    [[nodiscard]] bool uses_process() const noexcept override { return false; }

private:
    std::string name_;
    Body body_;
};

/// 构造这份练习的默认作业清单。
///
/// 这些作业是刻意挑的：每一条都在压一个具体的机制 ——
/// 大输出量压「必须持续抽管道」，自杀信号压「怎么区分退出码与终止信号」，
/// 长睡眠压「超时后必须整组杀掉」，不存在的二进制压「exec 成没成功」。
/// 全部只用 sh + coreutils，不依赖任何第三方程序。
std::vector<std::unique_ptr<Job>> make_default_jobs();

}  // namespace jobrun
