// ============================================================================
//  selfcheck.cpp —— 仓库自带的自检
//
//  它只做一件在单次运行里看不出来的事：把同一批作业在「同一个进程」里
//  反复跑很多轮，然后检查三件会随轮次累积的问题 ——
//    * 文件描述符是不是越用越多（fd 泄漏）
//    * 子进程是不是留下了僵尸（waitpid 还能不能捞到人）
//    * 每个作业的结局是不是始终符合预期（分类逻辑没有被并发搅乱）
//
//  【知识点：为什么单次运行测不出 fd 泄漏】进程一退，所有 fd 都由内核回收，
//  泄漏得再厉害也看不出来。只有在一个长命进程里反复跑，账才会显形。
//  这也是「单元测试」和「跑一遍看看」的根本差别。
// ============================================================================
#include <dirent.h>
#include <sys/wait.h>

#include <cstddef>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "jobrun/job.hpp"
#include "jobrun/report.hpp"
#include "jobrun/runner.hpp"

namespace {

int count_open_fds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (::readdir(dir) != nullptr) {
        ++count;
    }
    ::closedir(dir);
    return count - 2;  // 去掉 "." 和 ".."
}

/// 还有没有能捞到的子进程？全捞完会得到 ECHILD。
///
/// 【知识点：这是「没有僵尸」的直接证据】僵尸进程的特点正是
/// 「已经死了，但还能被 waitpid 捞到」。所以反复 WNOHANG 直到 ECHILD，
/// 就等价于「一个僵尸都不剩」。
bool no_children_left() {
    int status = 0;
    for (;;) {
        const pid_t r = ::waitpid(-1, &status, WNOHANG);
        if (r > 0) {
            continue;  // 竟然还有一个没回收的，继续捞
        }
        return r < 0;  // -1 且 ECHILD 才算干净；返回 0 表示还有孩子在跑
    }
}

struct Tally {
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::size_t signaled = 0;
    std::size_t timed_out = 0;
    std::size_t spawn_error = 0;
    std::size_t cancelled = 0;
    std::size_t total = 0;
};

int run_checks() {
    int failures = 0;
    auto check = [&failures](bool ok, const std::string& what) {
        std::cout << (ok ? "  [通过] " : "  [失败] ") << what << '\n';
        if (!ok) {
            ++failures;
        }
    };

    const int fds_before = count_open_fds();

    constexpr int kRounds = 6;
    Tally last;
    std::size_t first_total = 0;

    for (int round = 0; round < kRounds; ++round) {
        jobrun::RunnerConfig config;
        config.concurrency = 4;
        config.timeout_ms = 250;  // 让 timeout-trap 快点结束，测试跑得快一点
        config.queue_capacity = 16;

        jobrun::Runner runner(config);
        std::vector<std::unique_ptr<jobrun::Job>> jobs = jobrun::make_default_jobs();
        if (round == 0) {
            first_total = jobs.size();
        }

        // 【知识点：把标准输出临时换掉】std::cout 可以换一个背后的 streambuf。
        // 这里把 runner 的实时日志吸进一个 stringstream —— 测试输出才看得清楚。
        std::ostringstream sink;
        std::streambuf* saved = std::cout.rdbuf(sink.rdbuf());
        const jobrun::RunReport report = runner.run(std::move(jobs));
        std::cout.rdbuf(saved);

        last = Tally{};
        for (const jobrun::JobRecord& record : report.records) {
            ++last.total;
            switch (record.outcome.state) {
                case jobrun::RunState::Succeeded: ++last.succeeded; break;
                case jobrun::RunState::Failed: ++last.failed; break;
                case jobrun::RunState::Signaled: ++last.signaled; break;
                case jobrun::RunState::TimedOut: ++last.timed_out; break;
                case jobrun::RunState::SpawnError: ++last.spawn_error; break;
                case jobrun::RunState::Cancelled: ++last.cancelled; break;
            }
        }
    }

    // ---- 1. fd 账本 ----
    const int fds_after = count_open_fds();
    check(fds_before == fds_after,
          "跑了 " + std::to_string(kRounds) + " 轮之后 fd 数量没变（" + std::to_string(fds_before) + " -> " +
              std::to_string(fds_after) + "），没有 fd 泄漏");

    // ---- 2. 僵尸账本 ----
    check(no_children_left(), "一个子进程都没剩下，没有僵尸进程");

    // ---- 3. 分类账本 ----
    check(last.total == first_total, "每轮都提交了同样多的作业（" + std::to_string(last.total) + " 个）");
    check(last.succeeded == 7, "成功 7 个（实到 " + std::to_string(last.succeeded) + "）");
    check(last.failed == 1, "失败 1 个（实到 " + std::to_string(last.failed) + "）");
    check(last.signaled == 1, "被信号终止 1 个（实到 " + std::to_string(last.signaled) + "）");
    check(last.timed_out == 1, "超时 1 个（实到 " + std::to_string(last.timed_out) + "）");
    check(last.spawn_error == 1, "启动失败 1 个（实到 " + std::to_string(last.spawn_error) + "）");
    check(last.cancelled == 0, "没有被取消的作业（实到 " + std::to_string(last.cancelled) + "）");

    return failures;
}

}  // namespace

int main() {
    std::cout << "jobrun 自检\n";
    const int failures = run_checks();
    std::cout << (failures == 0 ? "\n全部通过。\n" : "\n有 " + std::to_string(failures) + " 项失败。\n");
    return failures == 0 ? 0 : 1;
}
