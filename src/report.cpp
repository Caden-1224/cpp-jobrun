// ============================================================================
//  report.cpp —— 报告渲染
//
// ============================================================================
//  【这个文件教什么】
//  ============================================================================
//  * operator<< 重载：让自定义类型像内置类型一样被打印
//  * std::ostringstream：在内存里拼字符串，再一次性输出
//  * iomanip 的 fixed / setprecision：控制浮点格式
//  * UTF-8 解码：首字节的高位决定字符长度
//  * 显示宽度：中文占两列，按字节补空格会把表格弄歪
//
// ============================================================================
//  【给只会 C 的你：printf 与 iostream 的取舍】
//  ============================================================================
//
//  printf("%-20s %5d", name, count) 的优点：格式串紧凑、性能好。
//  缺点：类型和格式串由你手工对齐，写错就是未定义行为；扩展自定义类型
//  更是完全没戏（只能加一层 snprintf 到缓冲区）。
//
//  C++ 的 operator<< 是「每个类型自己决定怎么打印」：
//      std::cout << stats;      // 调用我们定义的 operator<<
//  好处是类型安全、可扩展、可链式；代价是语法啰嗦、格式化控制不如 printf 直观。
//
//  本文件选择「先拼进 ostringstream，再一次性输出」：
//      * 同一份字符串既能 cout 也能写文件；
//      * 多线程下不会出现「两行输出交错」；
//      * 测试可以直接比较字符串。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. operator<< 重载
//         std::ostream& operator<<(std::ostream& os, RunState state) {
//             return os << to_string(state);
//         }
//     两个参数：输出流 + 要打印的值；返回流本身以支持链式。
//     【规则】如果第二个参数是自定义类型，ADL（参数相关查找）会在该类型
//            所属的命名空间里找 operator<<，所以 std::cout << state 能工作。
//
//  2. std::ostringstream
//     把输出写进内存字符串。str() 取出结果；构造/析构自动管理缓冲区。
//     【C 对照】C 里用 snprintf 到固定缓冲区，或者 asprintf（GNU 扩展），
//              再手动 free。
//
//  3. std::fixed / std::setprecision
//         out << std::fixed << std::setprecision(1) << kib;
//     fixed 表示「小数点后固定位数」，setprecision(1) 表示 1 位。
//     这两个是「粘性」的：设置一次，后续所有浮点输出都受影响，
//     所以要注意它们对同一流里后面内容的影响。
//
//  4. std::string(count, ch)
//         std::string(92, '-')   // 92 个 '-'
//         构造函数重载：(重复次数, 字符)。
//
//  5. 位运算解码 UTF-8
//         lead & 0x80  判断最高位
//         lead & 0xE0  取高 3 位
//         lead & 0xF0  取高 4 位
//     续字节固定以 10xxxxxx 开头，用 0x3F 取出低 6 位。
//
//  6. static_cast<unsigned char>
//     char 是否有符号由实现决定。做位运算前先转 unsigned char，
//     否则高位字节可能被符号扩展，0xE4 变成 0xFFFFFFE4。
//
// ============================================================================
//  【扩展：格式化还能怎么做】
//  ============================================================================
//  * std::format（C++20）：std::format("{:>20}", name)，类型安全 + 性能好。
//  * fmt 库：C++17 可用，API 与 std::format 基本一致。
//  * printf 家族：%-*s 可以动态指定宽度，但类型检查弱。
//  * 终端宽度：wcwidth(3) 按 Unicode East Asian Width 计算；本文件只做了
//    常用区间的保守判断，不处理组合字符、emoji 序列、双向文本。
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
///
/// 【语法】参数 (text, index, length)：
///         index 是当前字符首字节在 text 里的下标，length 是它的字节长度。
/// 【算法】
///         1. 只处理 3 字节的 UTF-8 序列（CJK 基本都在这个范围）；
///         2. 从首字节低 4 位 + 后两个续字节的低 6 位拼出码点；
///         3. 落在常见「全角/宽字符」区间就返回 2，否则 1。
std::size_t codepoint_width(const std::string& text, std::size_t index, std::size_t length) {
    if (length != 3 || index + 2 >= text.size()) {
        return 1;
    }
    // 【语法】位运算拼码点：
    //   首字节 1110xxxx  → (byte & 0x0F) << 12
    //   续字节 10xxxxxx  → (byte & 0x3F) << 6
    //   续字节 10xxxxxx  → (byte & 0x3F)
    // 【语法】static_cast<unsigned char> 防止 char 是 signed 时高位扩展。
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

/// 右补齐：在右边补空格，让显示宽度达到 columns。
///
/// 【语法】std::string(count, ' ')：构造 count 个空格。
std::string pad_right(const std::string& text, std::size_t columns) {
    const std::size_t width = display_width(text);
    if (width >= columns) {
        return text;
    }
    return text + std::string(columns - width, ' ');
}

/// 左补齐：在左边补空格。
std::string pad_left(const std::string& text, std::size_t columns) {
    const std::size_t width = display_width(text);
    if (width >= columns) {
        return text;
    }
    return std::string(columns - width, ' ') + text;
}

/// 毫秒格式，保留一位小数。
///
/// 【语法】和 runner.cpp 里的同名函数重复了。严格的做法是抽到一个公共头文件，
///         这里保留两份是为了让每个文件尽量自包含（教学项目里可读性优先）。
std::string format_ms(long long microseconds) {
    const long long tenths = microseconds / 100;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " ms";
}

/// 字节数 → 人类可读（B / KiB）。
///
/// 【语法】std::ostringstream 的典型用法：
///         out << std::fixed << std::setprecision(1) << kib << " KiB";
///         return out.str();
/// 【坑】std::fixed / setprecision 会一直影响这个流后续的输出，
///       所以这里用一个局部 ostringstream，不污染 std::cout。
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
    // 【语法】std::size_t 是无符号类型，适合表示下标/长度；sizeof 返回它。
    std::size_t width = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        // 【语法】static_cast<unsigned char> 后再和掩码比较，避免符号扩展。
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((lead & 0x80u) == 0) {
            // 0xxxxxxx：ASCII，1 字节
            length = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            // 110xxxxx：2 字节
            length = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            // 1110xxxx：3 字节（中文在这里）
            length = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            // 11110xxx：4 字节（emoji 等）
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

// 【语法】operator<< 的第一个参数是流，返回流引用以支持链式。
//         RunState 按值传；RunStats 按 const 引用传（大对象不拷贝）。
std::ostream& operator<<(std::ostream& os, RunState state) { return os << to_string(state); }

std::ostream& operator<<(std::ostream& os, const RunStats& stats) {
    // 【语法】连续 << 的返回值都是 os，所以能一路链下去。
    os << "总作业数 " << stats.total << "（开进程 " << stats.with_process << "，进程内 " << stats.builtin << "）\n";
    os << "成功 " << stats.succeeded << "，失败 " << stats.failed << "，被信号终止 " << stats.signaled
       << "，超时 " << stats.timed_out;
    if (stats.cancelled > 0 || stats.spawn_error > 0) {
        os << "，已取消 " << stats.cancelled << "，启动失败 " << stats.spawn_error;
    }
    os << '\n';
    os << "抓回 stdout " << stats.out_lines << " 行 / " << format_bytes(stats.out_bytes) << "，stderr "
       << stats.err_lines << " 行 / " << format_bytes(stats.err_bytes) << '\n';
    // 【语法】std::fixed + std::setprecision(2) 只影响之后的浮点输出。
    //         这里设置后，stats.speedup 就会按两位小数打印。
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

    // 【语法】range-for + const 引用：只读遍历，不拷贝 JobRecord。
    for (const JobRecord& record : report.records) {
        // 【语法】switch 覆盖 enum class 的六个值。
        //         每个 case 都 break，避免贯穿（fallthrough）。
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
        //
        // 【语法】static_cast<double> 防止整数除法截断。
        stats.speedup = static_cast<double>(stats.busy_us) / static_cast<double>(report.wall_us);
    }
    return stats;
}

std::string render_report(const RunReport& report, const RunStats& stats) {
    // 【语法】局部 ostringstream：拼完整份报告再返回，不直接碰 std::cout。
    std::ostringstream out;

    out << "============================== 作业档案（按提交顺序） ==============================\n";
    out << pad_left("#", 3) << "  " << pad_right("作业", 20) << pad_right("执行位置", 10)
        << pad_right("结局", 14) << pad_left("排队", 9) << "  " << pad_left("运行", 11) << "  "
        << pad_left("stdout", 13) << pad_left("stderr", 10) << '\n';
    // 【语法】std::string(92, '-')：92 个连字符，纯粹是分隔线。
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
    // 【语法】out << stats 会调用我们上面定义的 operator<<(ostream&, const RunStats&)。
    out << stats << '\n';
    if (report.interrupted) {
        out << "本次运行被中断信号打断过：未开工的作业被记为「已取消」。\n";
    }
    out << "提示：每次运行里作业的开始/结束顺序都可能不同，但上面这张表永远一致 ——\n"
        << "      因为它是按提交序号重排过的。这就是并发程序「过程不确定、结论确定」的做法。\n";

    // 【语法】str() 返回内部字符串的拷贝；C++20 有 std::move(out).str() 可以搬走。
    return out.str();
}

}  // namespace jobrun
