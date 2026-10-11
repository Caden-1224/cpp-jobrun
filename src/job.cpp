// ============================================================================
//  job.cpp —— 作业的实现，以及默认作业清单
// ============================================================================
#include "jobrun/job.hpp"

#include <chrono>
#include <cstring>
#include <numeric>
#include <utility>

namespace jobrun {
namespace {

using Clock = std::chrono::steady_clock;

long long since_us(Clock::time_point from) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - from).count();
}

std::string join(const std::vector<std::string>& parts) {
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            joined.push_back(' ');
        }
        joined += parts[i];
    }
    return joined;
}

JobOutcome from_capture(const CaptureResult& captured, const JobContext& context) {
    JobOutcome outcome;
    outcome.state = captured.state;
    outcome.duration_us = captured.duration_us;
    outcome.out_lines = captured.out_lines;
    outcome.err_lines = captured.err_lines;
    outcome.out_bytes = captured.out_bytes;
    outcome.err_bytes = captured.err_bytes;
    outcome.truncated = captured.truncated;
    outcome.out_preview = captured.out_preview;
    outcome.err_preview = captured.err_preview;

    switch (captured.state) {
        case RunState::SpawnError:
            // 【知识点：把 errno 翻成人话】strerror 不是线程安全的，
            // 但这里每个 worker 都在自己的线程里立刻用完就丢，没有共享。
            outcome.detail = std::string("无法启动：") + std::strerror(captured.spawn_errno);
            break;
        case RunState::TimedOut:
            outcome.detail = "超过 " + std::to_string(context.timeout_ms) + " ms，已终止整个进程组";
            break;
        case RunState::Cancelled:
            outcome.detail = "用户中断";
            break;
        default:
            outcome.detail = captured.status.describe();
            break;
    }
    return outcome;
}

}  // namespace

// ================================ ShellJob ================================

ShellJob::ShellJob(std::string name, std::vector<std::string> argv)
    : name_(std::move(name)), argv_(std::move(argv)) {}

std::string ShellJob::name() const { return name_; }

std::string ShellJob::command_line() const { return join(argv_); }

JobOutcome ShellJob::run(const JobContext& context) const {
    CaptureOptions options;
    options.timeout_ms = context.timeout_ms;
    options.max_preview_lines = context.max_preview_lines;
    options.stop = context.stop;
    return from_capture(run_command_captured(argv_, options), context);
}

// =============================== BuiltinJob ================================

BuiltinJob::BuiltinJob(std::string name, Body body) : name_(std::move(name)), body_(std::move(body)) {}

std::string BuiltinJob::name() const { return name_; }

std::string BuiltinJob::command_line() const { return "<进程内> " + name_; }

JobOutcome BuiltinJob::run(const JobContext& context) const {
    const auto started = Clock::now();
    JobOutcome outcome;
    try {
        // 【知识点：多态 + std::function】body_ 里装的是任意可调用对象。
        // 调用方不必知道它是一个 lambda、一个函数指针，还是一个绑定过的成员函数。
        std::vector<std::string> lines = body_();
        outcome.state = RunState::Succeeded;
        outcome.detail = "在当前线程内直接完成，未创建进程";
        outcome.out_lines = lines.size();
        for (const std::string& line : lines) {
            outcome.out_bytes += line.size();
            if (outcome.out_preview.size() < context.max_preview_lines) {
                outcome.out_preview.push_back(line);
            }
        }
    } catch (const std::exception& error) {
        // 【知识点：异常也是「作业失败」的一种】把它归一到和子进程失败
        // 同一个出口，上层就不必为两类作业写两套错误处理。
        outcome.state = RunState::Failed;
        outcome.detail = std::string("进程内抛出异常：") + error.what();
    }
    outcome.duration_us = since_us(started);
    return outcome;
}

// ============================ make_default_jobs ============================

std::vector<std::unique_ptr<Job>> make_default_jobs() {
    std::vector<std::unique_ptr<Job>> jobs;

    // 【知识点：make_unique + 基类指针】返回的是 unique_ptr<Job>，
    // 里面装的却是 ShellJob —— 这就是多态持有。
    // 由于 Job 有虚析构，unique_ptr<Job> 析构时会正确调用 ShellJob 的析构。
    jobs.push_back(std::make_unique<ShellJob>("echo-fast", std::vector<std::string>{"sh", "-c", "echo hello from jobrun"}));

    jobs.push_back(std::make_unique<ShellJob>(
        "stream-lines",
        std::vector<std::string>{"sh", "-c", "for i in 1 2 3 4 5; do echo \"逐行输出 line $i\"; sleep 0.05; done"}));

    jobs.push_back(std::make_unique<ShellJob>("exit-nonzero", std::vector<std::string>{"sh", "-c", "echo 我马上要以 3 退出; exit 3"}));

    // 自己给自己发 SIGTERM：走的是「被信号终止」而不是「退出码」那条路。
    jobs.push_back(std::make_unique<ShellJob>("killed-by-signal", std::vector<std::string>{"sh", "-c", "kill -TERM $$"}));

    // 约 1.2 MB 输出，远超 Linux 管道缓冲区（默认 64 KiB）。
    // 如果父进程不持续读，这个作业会在 write 上永久阻塞 —— 这正是必须抽管道的原因。
    jobs.push_back(std::make_unique<ShellJob>("big-stdout", std::vector<std::string>{"sh", "-c", "seq 1 200000"}));

    jobs.push_back(std::make_unique<ShellJob>(
        "stderr-mix",
        std::vector<std::string>{"sh", "-c", "echo 这是一行 stderr 1>&2; echo 这是一行 stdout"}));

    // 最后一行没有换行符：如果不特判，这一行会被漏掉。
    jobs.push_back(std::make_unique<ShellJob>(
        "no-trailing-newline", std::vector<std::string>{"sh", "-c", "printf '最后一行没有换行符'"}));

    // 长睡眠：sh 会 fork 出一个 sleep 孙子进程。
    // 只杀直接子进程的话，sleep 会活下来 —— 所以这里必须整组杀。
    jobs.push_back(std::make_unique<ShellJob>(
        "timeout-trap", std::vector<std::string>{"sh", "-c", "sleep 30; echo 这一行永远不该出现"}));

    // 一个 200 KB 的单行：触发「超长行截断」这条防御路径。
    jobs.push_back(std::make_unique<ShellJob>(
        "long-single-line",
        std::vector<std::string>{"sh", "-c", "head -c 200000 /dev/zero | tr '\\0' 'x'"}));

    // 直接 exec 一个不存在的二进制：走的是我们自己的 exec 探测管道，
    // 而不是 shell 报的 127。
    //
    // 【知识点：为什么这里故意写成绝对路径】如果只给一个裸名字，
    // execvp 会拿着 PATH 逐个目录去找。这台机器的 PATH 有 80 项，
    // 而且一半挂在 Windows 盘上（WSL 的 9p 文件系统，每次 stat 都要跨一次边界），
    // 一次注定失败的查找要花 200 ms 以上 —— 那会把这个作业真正的耗时彻底淹没，
    // 让人误以为是我们的代码慢。用绝对路径，剩下的原因就只有「文件不存在」这一条。
    jobs.push_back(std::make_unique<ShellJob>(
        "missing-binary", std::vector<std::string>{"/nonexistent/jobrun-no-such-binary"}));

    // 进程内作业：完全没有 fork，证明两类作业对上层是同一种东西。
    jobs.push_back(std::make_unique<BuiltinJob>("builtin-sum", [] {
        // 【知识点：lambda 捕获与 std::function】这个 lambda 捕获了 n，
        // 被装进 std::function 后再传给 BuiltinJob —— 类型被擦除了。
        constexpr long long n = 1'000'000;
        const long long sum = n * (n + 1) / 2;
        return std::vector<std::string>{
            "在进程内把 1.." + std::to_string(n) + " 加起来",
            "结果 = " + std::to_string(sum),
        };
    }));

    return jobs;
}

}  // namespace jobrun
