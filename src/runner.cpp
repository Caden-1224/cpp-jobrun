// ============================================================================
//  runner.cpp —— 线程池的实现
//
// ============================================================================
//  【这个文件教什么】
//  ============================================================================
//  * 匿名 namespace：文件私有的函数/类型
//  * lambda 的捕获列表：按引用 / 按值
//  * std::thread 的创建、join，以及「析构时还 joinable 会 terminate」
//  * std::mutex + std::lock_guard：保护共享的 cout 和 results
//  * std::atomic<bool>：跨线程的停止标志
//  * std::optional：pop 可能没有结果
//  * std::sort + 比较器：把乱序结果恢复成提交顺序
//
// ============================================================================
//  【给只会 C 的你：这段代码在 C 里对应什么】
//  ============================================================================
//
//  C + pthread：
//      pthread_create(&t, NULL, worker, &ctx);
//      pthread_join(t, NULL);
//  C++：
//      std::thread t(worker);   // 立即开始执行
//      t.join();                // 等它结束
//
//  关键差异：
//  1. std::thread 的构造函数会「拷贝/移动」可调用对象和参数。lambda 捕获的
//     是引用时，线程里拿到的也是引用 —— 所以必须保证被引用对象活到 join 之后。
//  2. std::thread 析构时如果还 joinable（没 join 也没 detach），会直接
//     std::terminate。pthread 里忘了 join 只是泄漏，C++ 里是当场崩溃。
//  3. 没有 pthread_mutex_init/destroy：std::mutex 构造即可用、析构自动清理。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. 匿名 namespace
//         namespace { ... }
//     里面的名字只在当前翻译单元可见，等价于 C 的 static 函数/变量。
//     【C 对照】static int helper(void); 在 C++ 里更推荐匿名 namespace，
//     因为它也能包住类型和模板。
//
//  2. lambda 捕获
//         [&log_mutex](const std::string& line) { ... }
//         [&]() { ... }
//     [&] 按引用捕获所有用到的外部变量；[=] 按值捕获。
//     【坑】按引用捕获的变量必须活得比 lambda 久。这里线程在 run() 返回前
//          都 join 了，所以安全。如果 detach 就会悬空。
//
//  3. std::optional 的 operator->
//         item->sequence
//     optional<T> 重载了 -> 和 *，可以直接访问 T 的成员。
//     前提是你已经确认它有值（前面判过 has_value）。
//
//  4. 移动进容器
//         results.push_back(std::move(record));
//     record 是局部变量，move 之后它的状态未指定，但可以安全析构。
//     对 JobRecord 这种带 string/vector 的类型，move 比拷贝便宜得多。
//
//  5. std::sort 的比较器
//         std::sort(begin, end, [](const A& a, const B& b) { return a.seq < b.seq; });
//     比较器必须满足「严格弱序」：irreflexive、传递、对称不可比。
//     写 <= 是常见错误（两个相等元素互相「小于」，排序行为未定义）。
//
//  6. std::atomic 的内存序
//         stop_.load(std::memory_order_relaxed)
//     relaxed 只保证「原子性 + 修改顺序」，不建立跨变量的 happens-before。
//     对「只看这一个标志」的用途足够。若还要保证「标志置位之前写的普通变量
//     对读线程可见」，应使用 store(release) / load(acquire)。
//
// ============================================================================
//  【本文件涉及的 C++ 标准库 API：参数与语义】
//  ============================================================================
//
//  template<class F, class... Args> explicit std::thread(F&& f, Args&&... args);
//      f     : 可调用对象（函数、lambda、绑定表达式……）
//      args  : 传给 f 的参数，按值 decay 后拷贝/移动进线程存储
//      行为  : 构造完成时线程已经开始执行（不保证立刻运行）
//      【规则】thread 对象析构前必须 join() 或 detach()，否则 terminate。
//      【扩展】std::jthread（C++20）：析构自动 join，并带 stop_token。
//
//  void std::thread::join();
//      阻塞直到线程结束；只能调用一次；不能 join 自己。
//  void std::thread::detach();
//      让线程在后台运行，thread 对象不再代表它。本项目不用 detach：
//      所有线程都必须活到 run() 结束。
//
//  std::lock_guard<std::mutex> guard(mutex);
//      构造 lock、析构 unlock；不可拷贝、不可移动。
//
//  T std::atomic<T>::load(memory_order) const;
//  void std::atomic<T>::store(T, memory_order);
//      memory_order_relaxed / acquire / release / acq_rel / seq_cst。
//
//  std::sort(RandomIt first, RandomIt last, Compare comp);
//      平均 O(n log n)；不稳定（相等元素顺序不保证）。
//      需要随机访问迭代器；std::list 要用 list::sort。
//
//  void std::_Exit(int status);
//      立即终止进程，不跑 atexit、不 flush stdio、不析构静态对象。
//      POSIX 的 _exit 行为相同。
//
// ============================================================================
//  【扩展：线程池还能怎么长】
//  ============================================================================
//  * std::jthread + std::stop_token：标准库自带协作取消。
//  * std::future / std::promise：把「返回值」从共享 vector 里拿出来，
//    调用方 future.get() 即可，不必自己加锁收集。
//  * 线程数别硬编码：std::thread::hardware_concurrency() 给核数提示。
//  * pthread_setname_np（Linux）给线程起名，top -H / gdb 里能看见。
//  * 想验证数据竞争，编译时加 -fsanitize=thread；想验证越界/泄漏加
//    -fsanitize=address,undefined。对并发项目，TSan 的收益最大。
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

// 【语法】using 别名，避免到处写 std::chrono::steady_clock。
using Clock = std::chrono::steady_clock;

// 【语法】duration_cast 把任意精度的时长转成微秒，再 .count() 取整数。
long long us_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
}

/// 只保留一位小数的毫秒，纯粹为了表格好看。
///
/// 【语法】整数除法截断：microseconds / 100 得到「十分之一毫秒」的整数部分。
///         然后用 /10 和 %10 拼出 "12.3" 这种形式。
/// 【坑】负数会让 % 的结果为负，这里所有输入都是非负的，所以没问题。
std::string format_ms(long long microseconds) {
    const long long tenths = microseconds / 100;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " ms";
}

/// 把序号补成两位，纯粹为了日志对齐。
///
/// 【语法】text.insert(text.begin(), ' ') 在开头插入一个字符。
///         std::string 支持迭代器区间，和 STL 算法兼容。
std::string pad2(std::size_t value) {
    std::string text = std::to_string(value);
    while (text.size() < 2) {
        text.insert(text.begin(), ' ');
    }
    return text;
}

/// 每种结局配一个符号，让日志一眼能扫。
///
/// 【语法】switch 的每个分支都 return，所以函数末尾不需要 return。
///         但有些编译器仍要求「所有路径都有返回值」，这里末尾的 return "?"
///         是防御性的（理论上不可达）。
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
///
/// 【语法】成员里有 std::unique_ptr<Job>，所以这个类型「不可拷贝、可移动」。
///         PendingJob(PendingJob&&) = default; 让编译器生成移动构造；
///         拷贝构造会被隐式删除（因为 unique_ptr 不可拷贝）。
struct PendingJob {
    std::size_t sequence = 0;
    std::unique_ptr<Job> job;
    Clock::time_point submitted_at;

    PendingJob() = default;
    // 【知识点：显式写出移动、隐式删掉拷贝】因为成员里有 unique_ptr，
    // 这个类型本来就不可拷贝。把移动显式写成 = default，
    // 既表达了意图，也让「它只能被搬走」这件事一目了然。
    //
    // 【语法】noexcept 很重要：标准容器在需要搬移元素时（例如 vector 扩容）
    //         会优先选择 noexcept 的移动构造；如果移动可能抛异常、而拷贝
    //         又已经被删除，就会直接编译失败。
    PendingJob(PendingJob&&) noexcept = default;
    PendingJob& operator=(PendingJob&&) noexcept = default;
};

}  // namespace

Runner::Runner(RunnerConfig config) : config_(config) {
    // 【语法】config 是按值传进来的，这里用拷贝初始化 config_。
    //         也可以写 : config_(std::move(config))；RunnerConfig 全是
    //         标量，拷贝和移动没区别，写成拷贝更直白。
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

    // 【语法】栈上构造：BoundedQueue 的析构在 run 返回时自动 close？
    //         不会 —— close 是显式调用的（见下面），析构只负责释放内部资源。
    //         这里必须显式 close，否则 worker 永远等不到「结束」信号。
    BoundedQueue<PendingJob> queue(config_.queue_capacity);
    std::mutex results_mutex;
    std::vector<JobRecord> results;
    std::mutex log_mutex;

    // 【知识点：std::cout 与多线程】每个 << 运算符调用本身不会数据竞争，
    // 但「拼一行」往往要调用好几次 <<，别的线程完全可能插进中间，
    // 于是两行输出糊在一起。这里的做法是：先在锁外把整行拼成一个 string，
    // 再在锁内一次性写出去 —— 锁的粒度越小越好。
    //
    // 【语法】[&log_mutex] 只按引用捕获这一个变量；其余变量没用到，不必捕获。
    // 【语法】参数 const std::string&：调用方传临时字符串也能绑定。
    auto emit = [&log_mutex](const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << line << '\n';
        // 【知识点】flush 让日志立刻可见；否则输出可能攒在缓冲区里，
        // 程序崩溃时看不到最后一行。
        std::cout.flush();
    };

    // 【语法】[&] 按引用捕获 queue、stop_、config_、results、results_mutex、emit。
    //         这个 lambda 会被拷贝进每个 std::thread；捕获的是引用，
    //         所以所有线程操作的是同一批对象。
    auto worker = [&]() {
        for (;;) {
            // 【语法】std::optional<PendingJob>：pop 可能返回 nullopt。
            //         这里按值接收，内部发生一次移动（PendingJob 只能移动）。
            std::optional<PendingJob> item = queue.pop();
            if (!item.has_value()) {
                return;  // 队列已关闭且已排空
            }

            const auto started = Clock::now();

            JobRecord record;
            // 【语法】item->sequence 等价于 (*item).sequence；
            //         optional 的 operator-> 让你像指针一样访问。
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
                // 【语法】虚函数调用：item->job 的静态类型是 Job*，
                //         实际指向 ShellJob 或 BuiltinJob，run() 动态分派。
                record.outcome = item->job->run(context);
            }

            record.wall_us = us_between(started, Clock::now());
            // 【语法】std::string("...") + marker(...) + ...：
            //         第一个操作数是 std::string，后面才能用 + 拼接 const char*。
            //         如果写成 "..." + marker(...) 就是指针相加，编译错误。
            emit(std::string("  ") + marker(record.outcome.state) + " #" + pad2(record.sequence) + " " +
                 record.name + " —— " + to_string(record.outcome.state) + "，" + record.outcome.detail +
                 "（" + format_ms(record.wall_us) + "）");

            {
                // 【语法】花括号限定 lock 的作用域：push_back 之后立刻解锁。
                std::lock_guard<std::mutex> lock(results_mutex);
                results.push_back(std::move(record));
            }
        }
    };

    emit("以 " + std::to_string(config_.concurrency) + " 个 worker 启动，待跑队列容量 " +
         std::to_string(config_.queue_capacity) + "，单作业时限 " + std::to_string(config_.timeout_ms) + " ms");

    std::vector<std::thread> workers;
    // 【语法】reserve 预分配，避免 emplace_back 过程中反复扩容。
    workers.reserve(config_.concurrency);
    for (std::size_t i = 0; i < config_.concurrency; ++i) {
        // 【语法】emplace_back(worker) 在 vector 里原地构造 std::thread，
        //         参数是 worker 这个 lambda（被拷贝一份）。
        //         构造完成时线程已经开始跑了。
        workers.emplace_back(worker);
    }

    SignalWatcher watcher;
    std::atomic<bool> monitor_done{false};
    // 【语法】[&] 捕获 watcher、monitor_done、stop_、emit。
    //         注意 monitor 线程在 run 返回前必须 join，否则它引用的一切都会悬空。
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
                //
                // 【语法】std::_Exit 不做任何清理，是异步安全、也最快的退出方式。
                //         用 130 是因为 shell 约定 128 + SIGINT(2) = 130。
                emit("再次收到中断信号，立即退出");
                std::cout.flush();
                std::_Exit(130);
            }
        }
    });

    std::size_t next_sequence = 0;
    // 【语法】for (std::unique_ptr<Job>& job : jobs) 用引用遍历，
    //         这样才能把 job move 出去（如果按值遍历，move 的只是副本）。
    for (std::unique_ptr<Job>& job : jobs) {
        PendingJob pending;
        pending.sequence = next_sequence++;
        pending.job = std::move(job);
        pending.submitted_at = Clock::now();
        // 【语法】queue.push(std::move(pending))：PendingJob 只能移动。
        //         如果队列已关闭，push 返回 false，生产者停止。
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
    // 【语法】monitor_done 置 true 后，monitor 最多再等 poll_once 的一个分片
    //         （100ms）就会看到并退出。
    monitor_done.store(true, std::memory_order_relaxed);
    monitor.join();

    // 把并发完成的乱序结果恢复成提交顺序。
    // 【语法】比较器返回 a.sequence < b.sequence，满足严格弱序。
    //         不要写 <=（两个相等元素会互相「小于」，std::sort 行为未定义）。
    std::sort(results.begin(), results.end(), [](const JobRecord& a, const JobRecord& b) {
        return a.sequence < b.sequence;
    });

    // 【语法】std::move(results) 把 vector 的内容搬进 report.records，
    //         避免一次 O(n) 的深拷贝。之后 results 处于有效但空的状态。
    report.records = std::move(results);
    report.wall_us = us_between(wall_start, Clock::now());
    report.interrupted = stop_.load(std::memory_order_relaxed);
    return report;
}

}  // namespace jobrun
