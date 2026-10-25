// ============================================================================
//  job.cpp —— 作业的实现，以及默认作业清单
//
// ============================================================================
//  【这个文件教什么】
// ============================================================================
//  * 成员初始化列表：构造函数的「构造」阶段
//  * std::move：把参数搬进成员，而不是拷贝
//  * switch 覆盖 enum class
//  * lambda：匿名函数，以及它怎么被装进 std::function
//  * try/catch：把「进程内作业抛异常」归一成「作业失败」
//  * std::make_unique + 基类指针：多态持有
//
// ============================================================================
//  【给只会 C 的你：几个容易混的点】
//  ============================================================================
//
//  1. 构造函数的冒号
//         ShellJob::ShellJob(std::string name, std::vector<std::string> argv)
//             : name_(std::move(name)), argv_(std::move(argv)) {}
//     冒号后面是「初始化」，函数体里是「赋值」。
//     对 std::string 来说，先默认构造再赋值 = 多一次分配；
//     用初始化列表 = 直接构造，少一次。对 const 成员和引用成员，
//     初始化列表是唯一可行的方式。
//     【坑】成员按「声明顺序」初始化，不是按列表顺序。name_ 在 argv_ 前面
//           声明，所以先初始化 name_。顺序写反了 -Wreorder 会警告。
//
//  2. std::move 不是「移动」
//     它只是把左值转成右值引用，让「移动构造」有机会被选中。
//     真正搬东西的是 std::string 的移动构造函数：
//         把内部指针搬过来，把源对象置空（通常是一个空串）。
//
//  3. lambda
//         [n] { return ...; }
//     [] 是捕获列表：值捕获 n，或者 [&] 引用捕获。lambda 本质是一个
//     编译器生成的类，捕获的变量是它的成员，operator() 是成员函数。
//     没有捕获的 lambda 可以退化成函数指针。
//
//  4. switch + enum class
//         switch (state) { case RunState::Succeeded: ... }
//     enum class 的成员必须带作用域。编译器能检查「有没有漏掉某个枚举值」
//     （-Wswitch），比 C 的裸 enum 安全。
//
//  5. strerror 不是线程安全的
//     它返回一个指向静态缓冲区的指针，多线程同时调用会互相覆盖。
//     本项目只在 worker 线程里「立刻用完就丢」，且没有共享那个缓冲区，
//     所以侥幸安全；严格的做法是用 strerror_r（POSIX）或 strerror_l。
//
// ============================================================================
//  【扩展】
//  ============================================================================
//  * 默认清单用 sh -c 是为了写法短，代价是多一层 shell 进程，而且字符串
//    需要自己保证引号转义正确。生产代码更常用 posix_spawn/execvp 直接传
//    argv（本项目正是这么做的），或者用 std::format 拼参数。
//  * C++20 的 std::format("结果 = {}", sum) 比字符串 + 拼接可读得多。
//  * C++23 的 std::expected 可以把「成功值 / 错误」合成一个返回值。
// ============================================================================
#include "jobrun/job.hpp"

#include <chrono>
#include <cstring>
#include <numeric>
#include <utility>

namespace jobrun {
namespace {

// 【语法】using 别名：Clock 就是 std::chrono::steady_clock。
//         steady_clock 是单调时钟，不受系统时间调整影响，适合测耗时；
//         system_clock 是墙上时钟，可以映射到日历时间。
using Clock = std::chrono::steady_clock;

// 【语法】auto 返回类型推导：编译器从 return 表达式推出 long long。
//         函数体里用 duration_cast 把「时钟差」转成微秒。
long long since_us(Clock::time_point from) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - from).count();
}

// 【语法】const std::vector<std::string>&：只读引用，不拷贝。
//         C 里你会传 const char *const *parts + size_t n。
std::string join(const std::vector<std::string>& parts) {
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            // 【语法】push_back(char) 追加一个字符；+= 可以追加 string 或 char。
            joined.push_back(' ');
        }
        joined += parts[i];
    }
    return joined;
}

// 【语法】按值返回 JobOutcome：结构体里有 vector<string>，
//         返回时会移动（或者被 RVO 省略），不会深拷贝。
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
            //
            // 【API】char *strerror(int errnum);
            //       返回指向静态缓冲区的指针，内容是 errno 对应的英文描述。
            //       失败时可能返回 "Unknown error N"。
            //       线程安全版本：strerror_r（POSIX）、strerror_l（glibc）。
            // 【扩展】C++ 里也可以抛 std::system_error(err, std::generic_category())，
            //       它的 what() 已经包含了同样的描述。
            outcome.detail = std::string("无法启动：") + std::strerror(captured.spawn_errno);
            break;
        case RunState::TimedOut:
            outcome.detail = "超过 " + std::to_string(context.timeout_ms) + " ms，已终止整个进程组";
            break;
        case RunState::Cancelled:
            outcome.detail = "用户中断";
            break;
        default:
            // 【语法】default 在这里兜住 Succeeded/Failed/Signaled。
            //         如果以后给 RunState 加了新值，编译器不会报错 ——
            //         这是 enum class + default 的一个取舍。
            outcome.detail = captured.status.describe();
            break;
    }
    return outcome;
}

}  // namespace

// ================================ ShellJob ================================

// 【语法】成员初始化列表 + std::move：
//         name 和 argv 是按值传进来的，用 move 搬进成员，避免拷贝。
//         声明顺序是 name_ 再 argv_，初始化列表也按这个顺序写。
ShellJob::ShellJob(std::string name, std::vector<std::string> argv)
    : name_(std::move(name)), argv_(std::move(argv)) {}

std::string ShellJob::name() const { return name_; }

std::string ShellJob::command_line() const { return join(argv_); }

JobOutcome ShellJob::run(const JobContext& context) const {
    // 【语法】聚合体初始化：CaptureOptions options; 会用到头文件里的默认成员值。
    //         然后逐字段覆盖需要改的部分。
    CaptureOptions options;
    options.timeout_ms = context.timeout_ms;
    options.max_preview_lines = context.max_preview_lines;
    options.stop = context.stop;
    // 【语法】按值返回：run_command_captured 返回临时 CaptureResult，
    //         直接绑定到 from_capture 的 const 引用参数上，生命周期延长到
    //         整个 from_capture 调用结束。
    return from_capture(run_command_captured(argv_, options), context);
}

// =============================== BuiltinJob ================================

// 【语法】Body 是 std::function<...>。按值传入，再 move 进成员：
//         如果调用方传的是 lambda 临时对象，这里只发生一次移动。
BuiltinJob::BuiltinJob(std::string name, Body body) : name_(std::move(name)), body_(std::move(body)) {}

std::string BuiltinJob::name() const { return name_; }

std::string BuiltinJob::command_line() const { return "<进程内> " + name_; }

JobOutcome BuiltinJob::run(const JobContext& context) const {
    const auto started = Clock::now();
    JobOutcome outcome;
    try {
        // 【知识点：多态 + std::function】body_ 里装的是任意可调用对象。
        // 调用方不必知道它是一个 lambda、一个函数指针，还是一个绑定过的成员函数。
        //
        // 【语法】body_() 就是调用 operator()；没有参数，返回 vector<string>。
        // 【坑】std::function 允许为空（默认构造），直接调用空 function 会抛
        //       std::bad_function_call。这里 body_ 一定非空（构造函数要求传入）。
        std::vector<std::string> lines = body_();
        outcome.state = RunState::Succeeded;
        outcome.detail = "在当前线程内直接完成，未创建进程";
        outcome.out_lines = lines.size();
        for (const std::string& line : lines) {
            // 【语法】range-for：只读遍历，line 是 const string&，不拷贝。
            outcome.out_bytes += line.size();
            if (outcome.out_preview.size() < context.max_preview_lines) {
                outcome.out_preview.push_back(line);
            }
        }
    } catch (const std::exception& error) {
        // 【知识点：异常也是「作业失败」的一种】把它归一到和子进程失败
        // 同一个出口，上层就不必为两类作业写两套错误处理。
        //
        // 【语法】catch (const std::exception&) 能接住标准库异常和所有
        //         公开继承自它的异常。what() 返回 const char*。
        //         【坑】catch (...) 能接住一切，但拿不到 what()。
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
    //
    // 【语法】jobs.push_back(std::make_unique<ShellJob>(...))：
    //         make_unique 把「new + 构造」合成一步，异常安全（构造失败不会泄漏）。
    //         C 里对应 malloc + 手动初始化；C++ 里 new 失败会抛 bad_alloc，
    //         不会返回 NULL。
    jobs.push_back(std::make_unique<ShellJob>("echo-fast", std::vector<std::string>{"sh", "-c", "echo hello from jobrun"}));

    jobs.push_back(std::make_unique<ShellJob>(
        "stream-lines",
        std::vector<std::string>{"sh", "-c", "for i in 1 2 3 4 5; do echo \"逐行输出 line $i\"; sleep 0.05; done"}));

    jobs.push_back(std::make_unique<ShellJob>("exit-nonzero", std::vector<std::string>{"sh", "-c", "echo 我马上要以 3 退出; exit 3"}));

    // 自己给自己发 SIGTERM：走的是「被信号终止」而不是「退出码」那条路。
    // 【语法】$$ 是 shell 的「当前进程 pid」，会被 sh 展开。
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
    // 【语法】字符串里的 '\\0' 是「反斜杠 + 0」两个字符，会被 sh 解释成
    //         NUL 字节；C++ 源码里写 "\\0" 才能得到这两个字符。
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
    //
    // 【语法】[] { ... } 是没有捕获的 lambda，它能隐式转换成函数指针，
    //         也能被 std::function 装下。
    // 【语法】1'000'000 是 C++14 的数字分隔符，纯为了可读性，值就是 1000000。
    // 【语法】constexpr 表示「编译期常量」，编译器可以据此做常量折叠。
    jobs.push_back(std::make_unique<BuiltinJob>("builtin-sum", [] {
        // 【知识点：lambda 捕获与 std::function】这个 lambda 捕获了 n，
        // 被装进 std::function 后再传给 BuiltinJob —— 类型被擦除了。
        constexpr long long n = 1'000'000;
        const long long sum = n * (n + 1) / 2;
        // 【语法】返回类型从 return 表达式推导：std::vector<std::string>。
        //         {} 列表初始化构造 vector，每个元素是 string。
        return std::vector<std::string>{
            "在进程内把 1.." + std::to_string(n) + " 加起来",
            "结果 = " + std::to_string(sum),
        };
    }));

    return jobs;
}

}  // namespace jobrun
