// ============================================================================
//  report.hpp —— 把一次运行的结果渲染成人看的报告
// ============================================================================
#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>

#include "jobrun/process.hpp"
#include "jobrun/runner.hpp"

namespace jobrun {

/// 从报告里榨出来的统计数字。
struct RunStats {
    std::size_t total = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::size_t signaled = 0;
    std::size_t timed_out = 0;
    std::size_t cancelled = 0;
    std::size_t spawn_error = 0;

    std::size_t with_process = 0;
    std::size_t builtin = 0;

    std::size_t out_lines = 0;
    std::size_t err_lines = 0;
    std::size_t out_bytes = 0;
    std::size_t err_bytes = 0;

    long long busy_us = 0;     ///< 所有作业耗时之和（多核下会大于墙钟时间）
    long long slowest_us = 0;
    std::string slowest_name;
    long long run_wall_us = 0; ///< 整批作业的墙钟时间
    std::size_t concurrency = 0;
    double speedup = 0.0;      ///< busy_us / run_wall_us：并发到底赚到了多少
};

RunStats summarize(const RunReport& report);

/// 【知识点：运算符重载】给自定义类型定义 <<，它就能像内置类型一样被打印。
/// 注意返回的是 ostream&，这样才能连着写 os << a << b。
std::ostream& operator<<(std::ostream& os, RunState state);
std::ostream& operator<<(std::ostream& os, const RunStats& stats);

/// 渲染完整报告（表格 + 输出样本 + 统计）。
std::string render_report(const RunReport& report, const RunStats& stats);

/// 计算一个 UTF-8 字符串在终端里占几列。
///
/// 【知识点：为什么需要这个】中文在等宽终端里占两列，按字节数补齐会让表格
/// 歪得没法看。UTF-8 是变长编码，所以必须逐码点扫描：首字节的高位 1 的个数
/// 就是整个字符的字节数。这是「文本」与「字节」之间最经典的一道坎。
std::size_t display_width(const std::string& text);

}  // namespace jobrun
