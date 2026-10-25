// ============================================================================
//  report.hpp —— 把一次运行的结果渲染成人看的报告
//
// ============================================================================
//  【给只会 C 的你：<< 不是「左移」吗】
//  ============================================================================
//
//  C 里 printf("%d", x) 靠格式串和可变参数；编译器不知道 x 的类型，
//  写错 %d/%s 就是未定义行为。
//  C++ 的 std::cout << x 是「运算符重载」：x 的类型决定了调用哪个版本的
//  operator<<。编译器会做类型检查，不需要格式串。
//
//  你可以为自己的类型定义 <<：
//      std::ostream& operator<<(std::ostream& os, RunState state);
//  之后 std::cout << RunState::Succeeded; 就能直接打印。
//  返回 ostream& 是为了支持链式：os << a << b << c;
//
//  ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. 返回 std::string 的渲染函数
//         std::string render_report(const RunReport&, const RunStats&);
//     先拼成完整字符串再一次性输出，好处是：
//         - 可以同时写屏幕和写文件（同一份内容）；
//         - 多线程下不会出现「两行输出交错」；
//         - 测试可以直接比较字符串。
//
//  2. 前置声明
//         #include <iosfwd>
//     iosfwd 只声明 std::ostream 等类型，不把整个 <iostream> 拖进来。
//     头文件里只需要「引用」，不需要「使用」时，这样能显著减少编译依赖。
//
//  3. std::size_t
//     无符号的「大小类型」，sizeof 的返回类型。C 里是 size_t，
//     C++ 标准库把它放在 std:: 里（全局也有，但建议写 std::）。
//
//  4. double
//     加速比是浮点数。注意 report.cpp 里输出前设置了 fixed + setprecision(2)，
//     否则默认只印 6 位有效数字。
//
// ============================================================================
//  【扩展：格式化输出还有哪些选择】
//  ============================================================================
//  * std::format（C++20）：printf 风格的格式串 + 类型安全，性能优于 iostream。
//  * fmt 库：std::format 的前身，C++17 可用。
//  * std::to_chars / std::from_chars（C++17）：无分配、无 locale 的数字转换。
//  * std::ostringstream：本项目用的方案，C++98 起就有，兼容性最好。
//  * std::print（C++23）：直接输出，不用先拼字符串。
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
///
/// 【语法】第一个参数是 std::ostream&，第二个是 RunState（按值）。
///         调用 std::cout << state 时，编译器在 jobrun 命名空间里找到这个
///         函数（ADL，参数相关查找），不需要你写 jobrun::operator<<。
/// 【扩展】C++20 还可以定义 operator<=> 一次性生成 ==、<、<=、>、>=。
std::ostream& operator<<(std::ostream& os, RunState state);
std::ostream& operator<<(std::ostream& os, const RunStats& stats);

/// 渲染完整报告（表格 + 输出样本 + 统计）。
///
/// 【语法】两个参数都是 const 引用：只读、不拷贝。
/// 【返回】std::string：值语义，调用方可以直接 cout / 写文件 / 比较。
std::string render_report(const RunReport& report, const RunStats& stats);

/// 计算一个 UTF-8 字符串在终端里占几列。
///
/// 【知识点：为什么需要这个】中文在等宽终端里占两列，按字节数补齐会让表格
/// 歪得没法看。UTF-8 是变长编码，所以必须逐码点扫描：首字节的高位 1 的个数
/// 就是整个字符的字节数。这是「文本」与「字节」之间最经典的一道坎。
///
/// 【C 对照】C 里 strlen 数的是字节，不是字符；C 标准库没有「显示宽度」的概念。
/// 【扩展】完整的终端宽度规则要看 Unicode East Asian Width 属性，甚至要考虑
///         组合字符、emoji（宽度 2）、零宽连接符。这里只做了保守的常用区间。
std::size_t display_width(const std::string& text);

}  // namespace jobrun
