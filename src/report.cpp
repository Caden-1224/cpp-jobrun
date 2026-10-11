// ============================================================================
//  report.cpp —— 报告渲染
// ============================================================================
#include "jobrun/report.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <sstream>

#include "jobrun/signals.hpp"

namespace jobrun {
namespace {

/// 把一行的字节数换算成「占几列」。规则保守一点没关系，错了也只是少几个空格。
std::size_t codepoint_width(const std::string& text, std::size_t index, std::size_t length) {
    if (length != 3 || index + 2 >= text.size()) {
        return 1;
    }
    const unsigned int cp = ((static_cast<unsigned char>(text[index]) & 0x0Fu) << 12) |
                            ((static_cast<unsigned char>(text[index + 1]) & 0x3Fu) << 6) |
                            (static_cast<unsigned char>(text[index + 2]) & 0x3Fu);
    const bool wide = (cp >= 0x1100 && cp <= 0x115F) ||   // 谚文
                      (cp >= 0x2E80 && cp <= 0xA4CF) ||   // 中日韩部首、假名、汉字
                      (cp >= 0xAC00 && cp <= 0xD7A3) ||   // 谚文音节
                      (cp >= 0xF900 && cp <= 0xFAFF) ||   // 兼容汉字
                      (cp >= 0xFE30 && cp <= 0xFE6F) ||   // 中日韩兼容形式
                      (cp >= 0xFF00 && cp <= 0xFF60) ||   // 全角形式
                      (cp >= 0xFFE0 && cp <= 0xFFE6);
    return wide ? 2 : 1;
}

std::string pad_right(const std::string& text, std::size_t columns) {
    const std::size_t width = display_width(text);
    if (width >= columns) {
        return text;
    }
    return text + std::string(columns - width, ' ');
}

std::string pad_left(const std::string& text, std::size_t columns) {
    const std::size_t width = display_width(text);
    if (width >= columns) {
        return text;
    }
    return std::string(columns - width, ' ') + text;
}

std::string format_ms(long long microseconds) {
    const long long tenths = microseconds / 100;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " ms";
}

std::string format_bytes(std::size_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }
    const double kib = static_cast<double>(bytes) / 1024.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << kib << " KiB";
    return out.str();
}

}  // namespace

std::size_t display_width(const std::string& text) {
    std::size_t width = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((lead & 0x80u) == 0) {
            length = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            length = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            length = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            length = 4;
        }
        // 防止把一个被截断的多字节序列读越界
        if (i + length > text.size()) {
            length = 1;
        }
        width += codepoint_width(text, i, length);
        i += length;
    }
    return width;
}

std::ostream& operator<<(std::ostream& os, RunState state) { return os << to_string(state); }

std::ostream& operator<<(std::ostream& os, const RunStats& stats) {
    os << "总作业数 " << stats.total << "（开进程 " << stats.with_process << "，进程内 " << stats.builtin << "）\n";
    os << "成功 " << stats.succeeded << "，失败 " << stats.failed << "，被信号终止 " << stats.signaled
       << "，超时 " << stats.timed_out;
    if (stats.cancelled > 0 || stats.spawn_error > 0) {
        os << "，已取消 " << stats.cancelled << "，启动失败 " << stats.spawn_error;
    }
    os << '\n';
    os << "抓回 stdout " << stats.out_lines << " 行 / " << format_bytes(stats.out_bytes) << "，stderr "
       << stats.err_lines << " 行 / " << format_bytes(stats.err_bytes) << '\n';
    os << "整批墙钟 " << format_ms(stats.run_wall_us) << "，所有作业耗时之和 " << format_ms(stats.busy_us)
       << "，并发度 " << stats.concurrency << "，加速比 " << std::fixed << std::setprecision(2) << stats.speedup
       << "×\n";
    if (!stats.slowest_name.empty()) {
        os << "最慢的作业 " << stats.slowest_name << "（" << format_ms(stats.slowest_us) << "）";
    }
    return os;
}

RunStats summarize(const RunReport& report) {
    RunStats stats;
    stats.total = report.records.size();
    stats.concurrency = report.concurrency;
    stats.run_wall_us = report.wall_us;

    for (const JobRecord& record : report.records) {
        switch (record.outcome.state) {
            case RunState::Succeeded: ++stats.succeeded; break;
            case RunState::Failed: ++stats.failed; break;
            case RunState::Signaled: ++stats.signaled; break;
            case RunState::TimedOut: ++stats.timed_out; break;
            case RunState::Cancelled: ++stats.cancelled; break;
            case RunState::SpawnError: ++stats.spawn_error; break;
        }
        if (record.uses_process) {
            ++stats.with_process;
        } else {
            ++stats.builtin;
        }
        stats.out_lines += record.outcome.out_lines;
        stats.err_lines += record.outcome.err_lines;
        stats.out_bytes += record.outcome.out_bytes;
        stats.err_bytes += record.outcome.err_bytes;
        stats.busy_us += record.wall_us;
        if (record.wall_us > stats.slowest_us) {
            stats.slowest_us = record.wall_us;
            stats.slowest_name = record.name;
        }
    }

    if (report.wall_us > 0) {
        // 【知识点：加速比为什么用「耗时之和 / 墙钟」】每个作业各自花了多久，
        // 加起来是「如果串行要多久」；除以真实墙钟，就是并发帮我们省了多少。
        // 它永远小于等于并发度，而且会因为排队和 I/O 等待而打折。
        stats.speedup = static_cast<double>(stats.busy_us) / static_cast<double>(report.wall_us);
    }
    return stats;
}

std::string render_report(const RunReport& report, const RunStats& stats) {
    std::ostringstream out;

    out << "============================== 作业档案（按提交顺序） ==============================\n";
    out << pad_left("#", 3) << "  " << pad_right("作业", 20) << pad_right("执行位置", 10)
        << pad_right("结局", 14) << pad_left("排队", 9) << "  " << pad_left("运行", 11) << "  "
        << pad_left("stdout", 13) << pad_left("stderr", 10) << '\n';
    out << std::string(92, '-') << '\n';

    for (const JobRecord& record : report.records) {
        const std::string out_cell =
            std::to_string(record.outcome.out_lines) + " 行 / " + format_bytes(record.outcome.out_bytes);
        const std::string err_cell =
            std::to_string(record.outcome.err_lines) + " 行";
        out << pad_left(std::to_string(record.sequence), 3) << "  " << pad_right(record.name, 20)
            << pad_right(record.uses_process ? "进程" : "进程内", 10)
            << pad_right(std::string(to_string(record.outcome.state)), 14)
            << pad_left(format_ms(record.queue_wait_us), 9) << "  " << pad_left(format_ms(record.wall_us), 11)
            << "  " << pad_left(out_cell, 13) << pad_left(err_cell, 10) << '\n';
    }

    out << "\n================================ 结局说明 ================================\n";
    for (const JobRecord& record : report.records) {
        out << "#" << record.sequence << " " << pad_right(record.name, 20) << record.outcome.detail << '\n';
    }

    out << "\n============================== 输出样本 ==============================\n";
    bool printed_any = false;
    for (const JobRecord& record : report.records) {
        if (record.outcome.out_preview.empty() && record.outcome.err_preview.empty()) {
            continue;
        }
        printed_any = true;
        out << "#" << record.sequence << " " << record.name << '\n';
        for (const std::string& line : record.outcome.out_preview) {
            out << "    [out] " << line << '\n';
        }
        for (const std::string& line : record.outcome.err_preview) {
            out << "    [err] " << line << '\n';
        }
        if (record.outcome.out_lines > record.outcome.out_preview.size() ||
            record.outcome.err_lines > record.outcome.err_preview.size()) {
            out << "    …（只保留前几行做样本，其余只计数）\n";
        }
        if (record.outcome.truncated) {
            out << "    （有超长行被截断）\n";
        }
    }
    if (!printed_any) {
        out << "（没有任何作业产生输出）\n";
    }

    out << "\n================================ 统计 ================================\n";
    out << stats << '\n';
    if (report.interrupted) {
        out << "本次运行被中断信号打断过：未开工的作业被记为「已取消」。\n";
    }
    out << "提示：每次运行里作业的开始/结束顺序都可能不同，但上面这张表永远一致 ——\n"
        << "      因为它是按提交序号重排过的。这就是并发程序「过程不确定、结论确定」的做法。\n";

    return out.str();
}

}  // namespace jobrun
