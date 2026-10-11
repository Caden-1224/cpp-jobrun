// ============================================================================
//  runner.hpp —— 线程池：把一堆作业并发地跑完
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "jobrun/job.hpp"

namespace jobrun {

struct RunnerConfig {
    std::size_t concurrency = 4;    ///< worker 线程数
    std::size_t queue_capacity = 16;///< 待跑队列的最大长度（有界，形成背压）
    int timeout_ms = 1000;          ///< 单个作业的时限
    std::size_t preview_lines = 3;  ///< 每个作业保留几行输出样本
};

/// 一个作业在本次运行里的完整档案。
struct JobRecord {
    std::size_t sequence = 0;   ///< 提交序号，用来恢复「提交顺序」
    std::string name;
    std::string command;
    bool uses_process = false;
    JobOutcome outcome;
    long long queue_wait_us = 0;  ///< 排队等了多久
    long long wall_us = 0;        ///< 真正跑了多久
};

struct RunReport {
    std::vector<JobRecord> records;  ///< 已按提交序号排好
    std::size_t concurrency = 0;
    std::size_t submitted = 0;
    long long wall_us = 0;
    bool interrupted = false;        ///< 是否被 Ctrl-C 打断过
};

/// 作业执行器：N 个 worker 线程 + 一个信号观察线程。
///
/// 【知识点：为什么结果要按序号重排】作业是并发完成的，完成顺序每次都不一样。
/// 如果直接按完成顺序出报告，同一个程序跑两次得到两份不同的表格 —— 没法看，
/// 也没法对照。所以我们在收集时记下提交序号，最后排一次序：
/// 「过程不确定，结论确定」，这是并发程序可测试性的基本手法。
class Runner {
public:
    explicit Runner(RunnerConfig config);
    ~Runner() = default;

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    /// 阻塞地把所有作业跑完，返回按提交顺序排好的报告。
    RunReport run(std::vector<std::unique_ptr<Job>> jobs);

    void request_stop() noexcept { stop_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool stop_requested() const noexcept { return stop_.load(std::memory_order_relaxed); }

private:
    RunnerConfig config_;
    std::atomic<bool> stop_{false};
};

}  // namespace jobrun
