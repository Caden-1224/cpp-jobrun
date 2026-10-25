// ============================================================================
//  main.cpp —— jobrun 的入口
//
// ============================================================================
//  【这个文件教什么】
//  ============================================================================
//  * main 的两种签名、argc/argv 的约定
//  * 参数解析：字符串比较、std::stoul / std::stoi、异常
//  * std::setw / std::left：iostream 的格式控制
//  * short write：write() 可能只写一半，必须循环
//  * 退出码：为什么 130 = 128 + SIGINT
//  * try/catch 包住 main：把异常变成退出码，而不是 terminate
//
// ============================================================================
//  【给只会 C 的你】
//  ============================================================================
//
//  C 的 main：
//      int main(int argc, char **argv) { ... }
//  C++ 的 main 多了几种合法写法：
//      int main()
//      int main(int argc, char **argv)
//      int main(int argc, char **argv, char **envp)   // 环境变量
//  返回值必须是 int；0/EXIT_SUCCESS 表示成功，非 0 表示失败。
//  argv[argc] 保证是 nullptr —— 这是标准规定的，所以可以写
//      for (char **p = argv; *p; ++p)
//
//  【C 对照】C 里参数解析通常手写 strcmp 链，或者用 getopt_long。
//  C++ 里可以：
//      * 继续用 getopt_long（POSIX，完全合法）；
//      * 手写 std::string 比较（本项目）；
//      * 用 CLI11 / lyra 之类的头文件库；
//      * C++17 的 std::string_view 做零拷贝比较。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. 匿名 namespace
//     Options / print_usage / parse_options / print_job_list / write_text_file
//     都只在 main.cpp 里可见，等价于 C 的 static。
//
//  2. std::stoul / std::stoi
//         std::stoul(str)  // string → unsigned long
//         std::stoi(str)   // string → int
//     失败时抛 std::invalid_argument（不是数字）或 std::out_of_range（溢出）。
//     【C 对照】C 的 strtol 返回 0 并设置 errno，还要自己检查 endptr。
//
//  3. lambda 捕获引用
//         auto value_of = [&](const char* flag) -> std::string { ... argv[++i]; ... };
//     [&] 捕获 i 和 argc 的引用，所以 lambda 里 ++i 会影响外层 for 循环。
//     这是参数解析里「吃掉下一个参数」的常用手法。
//
//  4. std::exit vs return
//         std::exit(0) 会跑 atexit、flush stdio、析构静态对象；
//         return 0 从 main 返回，效果类似但更「正常」。
//     这里 -h 走 std::exit(0)，是因为它在 parse_options 内部，没有返回值可传。
//
//  5. std::move 交出所有权
//         runner.run(std::move(jobs));
//     run 的参数是按值接收的 vector<unique_ptr<Job>>；
//     传左值会尝试拷贝（unique_ptr 不允许，编译失败），
//     传 std::move(jobs) 才会移动。
//
//  6. 异常处理
//         try { ... } catch (const std::exception& e) { ... return 2; }
//     标准库异常都公开继承自 std::exception，what() 给出一句话描述。
//     main 里兜底可以避免「异常逃出 main → std::terminate → 无提示崩溃」。
//
// ============================================================================
//  【本文件涉及的 API：参数与语义】
//  ============================================================================
//
//  ssize_t write(int fd, const void *buf, size_t count);
//      【规则】返回值可能小于 count（short write），必须循环直到写完；
//             返回 -1 + errno（EINTR 可重试，其它错误应放弃）。
//      【为什么不用 std::ofstream】这里刻意用裸 write，是为了展示 short write；
//             实际项目里 std::ofstream 更省心（它的 operator<< 内部帮你循环）。
//      【扩展】writev（一次写多个缓冲区）、pwrite（带偏移，不改变文件位置）、
//              fsync（落盘）、fdatasync。
//
//  unsigned long std::stoul(const std::string& str, size_t* pos = nullptr, int base = 10);
//      pos 非空时，返回「解析停在哪里」的下标；base 默认十进制。
//      失败抛 invalid_argument / out_of_range。
//
//  int std::stoi(const std::string& str, size_t* pos = nullptr, int base = 10);
//      同上，返回 int。
//
//  void std::exit(int status);
//      正常终止：调用 atexit 注册的函数、flush 并关闭 stdio、析构静态对象。
//      【坑】在信号处理器里不能用 exit（不是 async-signal-safe），要用 _Exit。
//
// ============================================================================
//  【扩展：参数解析的更好选择】
//  ============================================================================
//  * getopt_long（POSIX / glibc）：成熟、支持长选项，C 和 C++ 都能用。
//  * std::string_view（C++17）：比较参数时零拷贝，std::string_view arg = argv[i];
//  * std::from_chars（C++17）：不抛异常、不分配、无 locale 的数字解析。
//  * CLI11 / lyra / cxxopts：头文件库，声明式定义选项，适合选项多的程序。
// ============================================================================
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jobrun/fd.hpp"
#include "jobrun/job.hpp"
#include "jobrun/report.hpp"
#include "jobrun/runner.hpp"

namespace {

// 【语法】聚合体 + 默认成员初始化：Options opt; 直接可用。
struct Options {
    jobrun::RunnerConfig config;
    std::string report_path = "jobrun-report.txt";
    bool write_report = true;
    bool print_jobs = true;
};

void print_usage(const char* program) {
    // 【语法】连续 << 拼接；字符串字面量之间可以直接相邻书写，
    //         编译器会把 "a" "b" 合并成 "ab"（C 里也一样）。
    std::cout << "用法: " << program << " [选项]\n\n"
              << "选项:\n"
              << "  -j, --concurrency N  并发 worker 线程数（默认 4）\n"
              << "      --queue N        待跑队列容量，也就是背压上限（默认 16）\n"
              << "      --timeout MS     单个作业的时限，超时按进程组终止（默认 1000）\n"
              << "      --preview N      每个作业保留几行输出样本（默认 3）\n"
              << "      --report PATH    报告写入路径（默认 jobrun-report.txt）\n"
              << "      --no-report      不写报告文件\n"
              << "      --quiet          不打印作业清单\n"
              << "  -h, --help           显示本帮助\n\n"
              << "试一试:\n"
              << "  " << program << " -j 1            串行跑一遍，对比墙钟时间\n"
              << "  " << program << " -j 8            并发拉满，看加速比\n"
              << "  " << program << " --timeout 200   让更多作业掉进超时那条路\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    // 【语法】从 1 开始：argv[0] 是程序名。
    for (int i = 1; i < argc; ++i) {
        // 【语法】std::string 从 const char* 构造，之后就能用 ==、+ 等操作。
        const std::string arg = argv[i];
        // 【知识点：lambda 捕获引用】这个 lambda 捕获了 i 和 argc 的引用，
        // 所以它 ++i 的效果会体现在外层循环上 —— 参数解析里的常用手法。
        //
        // 【语法】-> std::string 是显式指定返回类型（也可以让编译器推导）。
        //         [&] 表示按引用捕获所有用到的外部变量。
        // 【坑】如果写成 [=]（按值捕获），++i 只改副本，外层循环就乱套了。
        auto value_of = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                // 【语法】std::string(flag) 把 const char* 转成 string，才能用 + 拼接。
                //         如果写 flag + " 后面需要一个值" 就是指针相加，编译错误。
                throw std::runtime_error(std::string(flag) + " 后面需要一个值");
            }
            // 【语法】前置 ++i：先自增，再取 argv[i] —— 吃掉下一个参数。
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            // 【语法】std::exit 需要 <cstdlib>；它会 flush stdio 并终止进程。
            std::exit(0);
        } else if (arg == "-j" || arg == "--concurrency") {
            // 【语法】static_cast<std::size_t> 显式转换，避免隐式窄化警告。
            //         std::stoul 返回 unsigned long，失败会抛异常。
            options.config.concurrency = static_cast<std::size_t>(std::stoul(value_of("--concurrency")));
        } else if (arg == "--queue") {
            options.config.queue_capacity = static_cast<std::size_t>(std::stoul(value_of("--queue")));
        } else if (arg == "--timeout") {
            const int timeout = std::stoi(value_of("--timeout"));
            if (timeout <= 0) {
                throw std::runtime_error("--timeout 必须是正数");
            }
            options.config.timeout_ms = timeout;
        } else if (arg == "--preview") {
            options.config.preview_lines = static_cast<std::size_t>(std::stoul(value_of("--preview")));
        } else if (arg == "--report") {
            options.report_path = value_of("--report");
        } else if (arg == "--no-report") {
            options.write_report = false;
        } else if (arg == "--quiet") {
            options.print_jobs = false;
        } else {
            throw std::runtime_error("不认识的参数：" + arg + "（用 --help 看用法）");
        }
    }
    return options;
}

void print_job_list(const std::vector<std::unique_ptr<jobrun::Job>>& jobs) {
    std::cout << "本次作业清单（共 " << jobs.size() << " 个）\n";
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        // 【语法】std::setw 只对「下一次」输出生效；std::left 是粘性的，
        //         设置一次后一直左对齐，直到改成 std::right。
        // 【语法】jobs[i]->name()：unique_ptr 的 operator-> 转发到 Job*，
        //         再通过虚函数调用子类实现。
        std::cout << "  #" << std::left << std::setw(3) << i << " " << std::setw(20) << jobs[i]->name()
                  << (jobs[i]->uses_process() ? "[进程]  " : "[进程内]") << jobs[i]->command_line() << '\n';
    }
    std::cout << '\n';
}

/// 把一段文本整个写进文件。
///
/// 【知识点：write 可能只写一半】write() 返回的是「实际写了多少字节」，
/// 它完全可能小于你要求的数量（磁盘满、遇到信号、管道场景下更常见）。
/// 所以必须循环到全部写完。这就是经典的 short write 问题。
///
/// 【语法】jobrun::Fd file = jobrun::Fd::create(path);
///         如果 open 失败会抛异常；file 离开作用域自动 close。
///         不需要手写 close，也不怕中间 return/throw。
void write_text_file(const std::string& path, const std::string& text) {
    jobrun::Fd file = jobrun::Fd::create(path);
    std::size_t written = 0;
    while (written < text.size()) {
        // 【语法】text.data() 返回 const char*（C++11 起 data() 也保证 '\0' 结尾）。
        //         text.data() + written 是指针算术，指向还没写的部分。
        // 【语法】ssize_t 是有符号的「大小/计数」类型，能表示 -1。
        const ssize_t n = ::write(file.get(), text.data() + written, text.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // 【语法】走到这里说明 n == 0（不该发生）或 n < 0 且不是 EINTR。
        //         fail() 会抛 std::system_error，异常展开时 file 自动 close。
        jobrun::fail("write(" + path + ")");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        // 【语法】const Options options = ...：
        //         const 局部变量在初始化后不可修改，编译器能做更多优化。
        const Options options = parse_options(argc, argv);

        // 【语法】auto 让编译器推导类型；这里显式写出 unique_ptr<Job> 更清楚。
        std::vector<std::unique_ptr<jobrun::Job>> jobs = jobrun::make_default_jobs();
        if (options.print_jobs) {
            print_job_list(jobs);
        }

        jobrun::Runner runner(options.config);
        // 【知识点：std::move 交出所有权】run 之后 jobs 里就是一堆空指针了，
        // 再碰它就是未定义行为 —— 这正是 unique_ptr 要表达的意思：
        // 「这个东西只有一个主人，交出去之后你就没有了」。
        const jobrun::RunReport report = runner.run(std::move(jobs));

        const jobrun::RunStats stats = jobrun::summarize(report);
        const std::string text = jobrun::render_report(report, stats);
        std::cout << '\n' << text;

        if (options.write_report) {
            write_text_file(options.report_path, text);
            std::cout << "报告已写入 " << options.report_path << '\n';
        }

        const std::size_t unfinished =
            stats.failed + stats.signaled + stats.timed_out + stats.spawn_error + stats.cancelled;
        if (report.interrupted) {
            // 【知识点：退出码的惯例】被信号打断时返回 128 + 信号号。
            // shell 里 $? 拿到 130，一眼就能看出「不是自己退出的，是被 Ctrl-C 打断的」。
            std::cout << "\n运行被中断，按惯例以 130 退出（128 + SIGINT）。\n";
            return 130;
        }
        if (unfinished != 0) {
            // 【知识点：退出码也是接口】管道、CI、脚本都靠它判断成败。
            // 默认清单里有几个故意失败的作业，所以这个程序正常跑完也会返回 1。
            std::cout << "\n有 " << unfinished << " 个作业没能成功完成，进程退出码 1。\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        // 【语法】catch 按引用接收，避免切片（slicing）并保留动态类型。
        //         如果写成 catch (std::exception error) 按值捕获，
        //         system_error 的派生部分会被切掉。
        std::cerr << "jobrun: " << error.what() << '\n';
        // 【约定】2 表示「程序自己出错」，和「作业失败 1」「被中断 130」区分开。
        return 2;
    }
}
