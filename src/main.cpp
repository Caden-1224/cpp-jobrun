// ============================================================================
//  main.cpp —— jobrun 的入口
//
//  【知识点：main 的两种签名】int main(int argc, char** argv) 是带参数的那一种。
//  argc 是参数个数（含程序名），argv 是字符串数组，argv[argc] 保证是 nullptr。
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

struct Options {
    jobrun::RunnerConfig config;
    std::string report_path = "jobrun-report.txt";
    bool write_report = true;
    bool print_jobs = true;
};

void print_usage(const char* program) {
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
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        // 【知识点：lambda 捕获引用】这个 lambda 捕获了 i 和 argc 的引用，
        // 所以它 ++i 的效果会体现在外层循环上 —— 参数解析里的常用手法。
        auto value_of = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string(flag) + " 后面需要一个值");
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "-j" || arg == "--concurrency") {
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
void write_text_file(const std::string& path, const std::string& text) {
    jobrun::Fd file = jobrun::Fd::create(path);
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(file.get(), text.data() + written, text.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        jobrun::fail("write(" + path + ")");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);

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
        std::cerr << "jobrun: " << error.what() << '\n';
        return 2;
    }
}
