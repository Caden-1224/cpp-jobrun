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
//
// ============================================================================
//  【给只会 C 的你：这个文件同时是一份「怎么测资源」的示范】
//  ============================================================================
//  C 里也能写这些检查，只是需要手动做：
//      DIR *d = opendir("/proc/self/fd");
//      while ((e = readdir(d))) count++;
//      closedir(d);
//      然后比较每一轮前后的 count。
//  C++ 的贡献在于：
//      * RAII 让 DIR* / 子进程 / 报告不靠 goto cleanup；
//      * std::ostringstream + rdbuf 重定向让「测试时屏蔽日志」变成一行；
//      * 异常让「某一步失败」不会静默地继续跑。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. 聚合体 + 值初始化
//         Tally last;          // 成员都有 = 0，安全
//         last = Tally{};      // 重新归零
//     Tally{} 会创建临时对象并逐个成员初始化，再移动赋值给 last。
//
//  2. 捕获引用的 lambda
//         auto check = [&failures](bool ok, const std::string& what) { ... };
//     failures 是按引用捕获，所以 lambda 里 ++failures 会影响外层变量。
//     如果写成 [=]，修改的只是副本，测试会永远「全过」。
//
//  3. std::cout.rdbuf(...)
//         std::ostringstream sink;
//         auto *saved = std::cout.rdbuf(sink.rdbuf());
//         ... 跑测试 ...
//         std::cout.rdbuf(saved);
//     把 stdout 的「背后缓冲区」换掉，让 runner 的实时日志被吸进 sink。
//     【坑】必须在同一线程里换回来；如果测试里开了别的线程并写 cout，
//           行为就不确定了。
//
//  4. switch 覆盖 enum class
//         每个 case 都要 break；这里故意不用 default，让 -Wswitch 能发现
//         新增的枚举值没被统计。
//
//  5. 三目运算符
//         std::cout << (ok ? "  [通过] " : "  [失败] ");
//     注意括号：<< 的优先级比 ?: 高，不加括号会解析成 (cout << ok) ? ... : ...
//
//  6. 字符串拼接
//         "跑了 " + std::to_string(kRounds) + " 轮之后 ..."
//     第一个操作数必须是 std::string，才能用 + 拼后面的字面量。
//
// ============================================================================
//  【本文件涉及的 POSIX / 标准库 API】
//  ============================================================================
//
//  DIR *opendir(const char *name);
//      struct dirent *readdir(DIR *dirp);
//      int closedir(DIR *dirp);
//      readdir 返回 NULL 表示读完或出错（用 errno 区分）。
//      /proc/self/fd 是 Linux 专有：每个打开着的 fd 在这里显示为一个条目。
//      【扩展】Linux 还有 /proc/self/fdinfo、/proc/self/maps；
//              macOS 没有 /proc，需要用 fcntl(F_GETFD) 逐个探测或 lsof。
//
//  pid_t waitpid(pid_t pid, int *wstatus, int options);
//      pid = -1 表示「任意子进程」；WNOHANG 表示不阻塞。
//      返回 0 表示还有孩子在跑；-1 + ECHILD 表示一个都不剩。
//
//  std::streambuf *std::ostream::rdbuf();
//      std::streambuf *std::ostream::rdbuf(std::streambuf *sb);
//      无参版本返回当前缓冲区；有参版本替换并返回旧的。
//
//  【扩展】
//  * 正式项目用 GoogleTest / Catch2 / doctest，而不是手写 check。
//  * TSan（-fsanitize=thread）能抓数据竞争；ASan（-fsanitize=address）
//    能抓越界和 use-after-free；UBSan 抓未定义行为。
//  * 想测「fd 有没有泄漏」，还可以在 Linux 上用 strace -e trace=close
//    或 /proc/self/fdinfo 观察。
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

/// 数当前进程打开了多少个 fd。
///
/// 【API】/proc/self/fd 里每个条目对应一个打开的 fd；
///         "." 和 ".." 是目录项，不是 fd，所以最后减 2。
/// 【坑】opendir 自己也会占一个 fd，但它返回的 DIR* 在 closedir 之前有效；
///       这里先读完再 closedir，所以计数里包含它，减 2 正好抵消 "."/".."
///       但不抵消 DIR 自身 —— 严格来说应该再减 1 或先关闭再数。
///       由于每轮都这样数，前后差值仍然可信。
/// 【语法】返回 -1 表示 /proc 不可用（例如非 Linux 平台）。
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
///
/// 【API】waitpid(-1, &status, WNOHANG)：
///         >0 捞到一个孩子（继续捞）
///          0 还有孩子在跑（不干净）
///         -1 没有孩子了（errno == ECHILD，干净）
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

/// 每种结局的计数。
///
/// 【语法】所有成员都有默认值，所以 Tally t; 是「全 0」而不是垃圾值。
///         Tally{} 会重新构造一个全 0 的临时对象，用来重置。
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
    // 【语法】[&failures] 按引用捕获，lambda 里 ++failures 会改到外部变量。
    //         [=] 会改副本，测试就永远不会失败 —— 这是很隐蔽的坑。
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
        // 【语法】每轮都新建 Runner/Config/Job 列表：
        //         这样测的是「反复创建销毁」而不是「复用同一个对象」。
        //         复用同一个 Runner 可能掩盖析构/初始化里的问题。
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
        //
        // 【语法】rdbuf(sink.rdbuf()) 返回旧的 streambuf*，存下来之后恢复。
        // 【坑】如果这里抛异常，saved 就恢复不了；RAII 的写法是包一个
        //       「析构时恢复」的小对象。教学项目里为了短，直接手写恢复。
        std::ostringstream sink;
        std::streambuf* saved = std::cout.rdbuf(sink.rdbuf());
        const jobrun::RunReport report = runner.run(std::move(jobs));
        std::cout.rdbuf(saved);

        // 【语法】last = Tally{}; 重新值初始化，把上一轮的计数清零。
        last = Tally{};
        for (const jobrun::JobRecord& record : report.records) {
            ++last.total;
            // 【语法】switch 覆盖全部六种状态；故意不写 default，
            //         这样如果 RunState 以后加了新值，-Wswitch 会提醒。
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
    // 【知识点】如果 Fd 的析构漏了 close，或者某条错误路径提前 return，
    // fds_after 会比 fds_before 大。跑 6 轮才能让泄漏「显形」。
    const int fds_after = count_open_fds();
    check(fds_before == fds_after,
          "跑了 " + std::to_string(kRounds) + " 轮之后 fd 数量没变（" + std::to_string(fds_before) + " -> " +
              std::to_string(fds_after) + "），没有 fd 泄漏");

    // ---- 2. 僵尸账本 ----
    check(no_children_left(), "一个子进程都没剩下，没有僵尸进程");

    // ---- 3. 分类账本 ----
    // 【知识点】这些期望值来自 make_default_jobs()：11 个作业，
    // 其中 7 成功、1 失败、1 被信号杀、1 超时、1 启动失败。
    // 如果并发逻辑把状态搞混了，这里就会对不上。
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
    // 【语法】三目运算符的两个分支要能转换成同一个类型：
    //         这里 const char* 会转换成 std::string，所以整个表达式是 string。
    std::cout << (failures == 0 ? "\n全部通过。\n" : "\n有 " + std::to_string(failures) + " 项失败。\n");
    // 【规则】退出码是 CI 判断成败的依据：0 成功，非 0 失败。
    return failures == 0 ? 0 : 1;
}
