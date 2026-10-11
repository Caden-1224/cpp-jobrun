// ============================================================================
//  runner.cpp —— 线程池的实现
// ============================================================================
#include "jobrun/runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "jobrun/queue.hpp"
#include "jobrun/signals.hpp"

namespace jobrun {
namespace {

using Clock = std::chrono::steady_clock;

long long us_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
}

/// 只保留一位小数的毫秒，纯粹为了表格好看。
std::string format_ms(long long microseconds) {
    const long long tenths = microseconds / 100;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " ms";
}

std::string pad2(std::size_t value) {
    std::string text = std::to_string(value);
    while (text.size() < 2) {
        text.insert(text.begin(), ' ');
    }
    return text;
}

const char* marker(RunState state) {
    switch (state) {
        case RunState::Succeeded: return "✔";
        case RunState::Failed: return "✘";
        case RunState::Signaled: return "☠";
        case RunState::TimedOut: return "⏱";
        case RunState::Cancelled: return "⏹";
        case RunState::SpawnError: return "⚠";
    }
    return "?";
}

/// 队列里流动的东西：一个作业 + 它的提交序号 + 提交时刻。
struct PendingJob {
    std::size_t sequence = 0;
    std::unique_ptr<Job> job;
    Clock::time_point submitted_at;

    PendingJob() = default;
    // 【知识点：显式写出移动、隐式删掉拷贝】因为成员里有 unique_ptr，
    // 这个类型本来就不可拷贝。把移动显式写成 = default，
    // 既表达了意图，也让「它只能被搬走」这件事一目了然。
    PendingJob(PendingJob&&) noexcept = default;
    PendingJob& operator=(PendingJob&&) noexcept = default;
};

}  // namespace

Runner::Runner(RunnerConfig config) : config_(config) {
    if (config_.concurrency == 0) {
        config_.concurrency = 1;
    }
    if (config_.queue_capacity < config_.concurrency) {
        config_.queue_capacity = config_.concurrency;
    }
}

RunReport Runner::run(std::vector<std::unique_ptr<Job>> jobs) {
    const auto wall_start = Clock::now();

    RunReport report;
    report.concurrency = config_.concurrency;
    report.submitted = jobs.size();

    BoundedQueue<PendingJob> queue(config_.queue_capacity);
    std::mutex results_mutex;
    std::vector<JobRecord> results;
    std::mutex log_mutex;

    // 【知识点：std::cout 与多线程】每个 << 运算符调用本身不会数据竞争，
    // 但「拼一行」往往要调用好几次 <<，别的线程完全可能插进中间，
    // 于是两行输出糊在一起。这里的做法是：先在锁外把整行拼成一个 string，
    // 再在锁内一次性写出去 —— 锁的粒度越小越好。
    auto emit = [&log_mutex](const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << line << '\n';
        std::cout.flush();
    };

    auto worker = [&]() {
        for (;;) {
            std::optional<PendingJob> item = queue.pop();
            if (!item.has_value()) {
                return;  // 队列已关闭且已排空
            }

            const auto started = Clock::now();

            JobRecord record;
            record.sequence = item->sequence;
            record.name = item->job->name();
            record.command = item->job->command_line();
            record.uses_process = item->job->uses_process();
            record.queue_wait_us = us_between(item->submitted_at, started);

            if (stop_.load(std::memory_order_relaxed)) {
                // 已经收到 Ctrl-C：剩下的作业不再开工，如实记为「已取消」。
                record.outcome.state = RunState::Cancelled;
                record.outcome.detail = "用户中断，尚未开工";
            } else {
                emit("  ▶ #" + pad2(record.sequence) + " " + record.name + " 开跑");
                JobContext context;
                context.timeout_ms = config_.timeout_ms;
                context.max_preview_lines = config_.preview_lines;
                context.stop = &stop_;
                record.outcome = item->job->run(context);
            }

            record.wall_us = us_between(started, Clock::now());
            emit(std::string("  ") + marker(record.outcome.state) + " #" + pad2(record.sequence) + " " +
                 record.name + " —— " + to_string(record.outcome.state) + "，" + record.outcome.detail +
                 "（" + format_ms(record.wall_us) + "）");

            {
                std::lock_guard<std::mutex> lock(results_mutex);
                results.push_back(std::move(record));
            }
        }
    };

    emit("以 " + std::to_string(config_.concurrency) + " 个 worker 启动，待跑队列容量 " +
         std::to_string(config_.queue_capacity) + "，单作业时限 " + std::to_string(config_.timeout_ms) + " ms");

    std::vector<std::thread> workers;
    workers.reserve(config_.concurrency);
    for (std::size_t i = 0; i < config_.concurrency; ++i) {
        workers.emplace_back(worker);
    }

    SignalWatcher watcher;
    std::atomic<bool> monitor_done{false};
    std::thread monitor([&]() {
        int received = 0;
        while (!monitor_done.load(std::memory_order_relaxed)) {
            const int sig = watcher.poll_once(100);
            if (sig == 0) {
                continue;
            }
            ++received;
            if (received == 1) {
                stop_.store(true, std::memory_order_relaxed);
                emit(std::string("收到 ") + signal_name(sig) +
                     "：停止派发新作业，并终止所有正在运行的进程组…");
            } else {
                // 【知识点：优雅退出必须留一个逃生口】第一次 Ctrl-C 走收工流程，
                // 第二次直接硬退。否则一个卡死的作业会让你只能开另一个终端来 kill。
                emit("再次收到中断信号，立即退出");
                std::cout.flush();
                std::_Exit(130);
            }
        }
    });

    std::size_t next_sequence = 0;
    for (std::unique_ptr<Job>& job : jobs) {
        PendingJob pending;
        pending.sequence = next_sequence++;
        pending.job = std::move(job);
        pending.submitted_at = Clock::now();
        if (!queue.push(std::move(pending))) {
            break;  // 队列已关闭，说明正在收工
        }
    }

    // 【知识点：关闭顺序决定会不会挂死】先 close 再 join：
    // close 唤醒所有阻塞在 pop 上的 worker，它们排空队列后自然退出。
    // 反过来先 join 的话，worker 永远等不到新元素，join 也永远不返回。
    queue.close();
    for (std::thread& t : workers) {
        t.join();
    }
    monitor_done.store(true, std::memory_order_relaxed);
    monitor.join();

    // 把并发完成的乱序结果恢复成提交顺序。
    std::sort(results.begin(), results.end(), [](const JobRecord& a, const JobRecord& b) {
        return a.sequence < b.sequence;
    });

    report.records = std::move(results);
    report.wall_us = us_between(wall_start, Clock::now());
    report.interrupted = stop_.load(std::memory_order_relaxed);
    return report;
}

}  // namespace jobrun
