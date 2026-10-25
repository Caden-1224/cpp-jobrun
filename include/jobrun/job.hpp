// ============================================================================
//  job.hpp —— 作业的抽象接口，以及两种具体的作业
//
// ============================================================================
//  【这个文件教什么】
// ============================================================================
//
//  一个「作业」可以是外部命令，也可以是一段进程内逻辑。对上层来说它们没有
//  区别：都能被命名、被打印、被执行。这个「同一种接口，不同的实现」就是
//  C++ 的运行时多态。
//
// ============================================================================
//  【给只会 C 的你：C 里怎么做多态】
// ============================================================================
//
//  C 的经典写法是「结构体 + 函数指针表」：
//
//      struct Job;
//      struct JobVTable {
//          const char *(*name)(const struct Job *);
//          int (*run)(const struct Job *, const struct JobContext *);
//          void (*destroy)(struct Job *);
//      };
//      struct Job {
//          const struct JobVTable *vtable;
//          const char *name;
//          void *impl;             // 子类自己的数据
//      };
//
//      // 调用：job->vtable->run(job, ctx);
//
//  这套东西在 C 里能工作，但每个细节都要手工维护：vtable 谁初始化、
//  destroy 谁调用、impl 什么时候释放、类型转换放在哪里。
//
//  C++ 用三个关键字把它变成语言特性：
//      virtual  声明「这个函数可以在子类里被替换」
//      override 声明「我确实在替换基类的虚函数」（写错签名会编译报错）
//      final    声明「到此为止，不许再派生」
//
//  编译器会在每个对象里放一个隐藏的 vptr，指向该类的 vtable。
//  对象大小会多一个指针；一次虚函数调用是一次间接跳转（通常无法内联）。
//  这就是多态的代价 —— 换来的是「上层代码完全不需要知道子类存在」。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
// ============================================================================
//
//  1. 纯虚函数与抽象类
//         virtual std::string name() const = 0;
//     = 0 表示「这里没有实现，子类必须提供」。含纯虚函数的类是抽象类，
//     Job j; 直接编译失败 —— 只能通过指针/引用使用基类。
//
//  2. 虚析构
//         virtual ~Job() = default;
//     通过 Job* 删除 ShellJob 时，如果析构不是虚的，只会调用 Job 的析构，
//     ShellJob 的成员（std::string、vector）不会被释放。
//     【规则】只要一个类打算被继承并通过基类指针删除，析构就必须虚。
//
//  3. override / final
//         std::string name() const override;
//         class ShellJob final : public Job;
//     override 让「签名写错」从「悄悄没覆盖」变成编译错误。
//     final 禁止继续派生，也给编译器优化（去虚化）的机会。
//
//  4. using 类型别名
//         using Body = std::function<std::vector<std::string>()>;
//     等价于 typedef，但可读性更好，而且能配合模板。
//
//  5. std::function
//     可以装下任何「签名匹配的可调用对象」：函数指针、lambda、绑定过的
//     成员函数、带 operator() 的类。它靠类型擦除实现，代价是一次间接调用
//     加可能的堆分配。
//     【C 对照】C 里只能存一个函数指针 + 一个 void* 上下文，调用时手动转型。
//
//  6. 按值返回 std::vector<std::unique_ptr<Job>>
//     返回容器时用移动而不是拷贝（编译器会自动 move 局部变量）。
//     unique_ptr 不可拷贝，所以这个 vector 本身也只能移动。
//
//  7. const 成员函数
//         std::string name() const;
//     承诺不修改对象状态。const Job& 只能调用 const 成员函数。
//     这是「接口的只读性」在类型系统里的表达，C 里只能靠注释约定。
//
//  8. [[nodiscard]] 与虚函数
//         [[nodiscard]] virtual std::string name() const = 0;
//     属性写在 virtual 前面，对覆盖它的所有子类都生效。
//
// ============================================================================
//  【扩展：多态还有别的写法】
//  ============================================================================
//  * std::variant<ShellJob, BuiltinJob> + std::visit：封闭集合的多态
//    （标签联合），没有虚表、没有堆分配，但类型集合必须封闭。
//  * CRTP（奇异递归模板模式）：基类模板参数是子类自己，编译期静态分派，
//    常见于「零开销接口」。
//  * std::move_only_function（C++23）：可以装 unique_ptr 这类只能移动的可调用对象，
//    std::function 要求可拷贝，装不了。
//  * 函数指针 + void* 上下文：C 风格，仍然可用，但没有类型安全。
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "jobrun/process.hpp"

namespace jobrun {

/// 一个作业跑完之后留下的一切。
///
/// 【语法】struct 默认 public，所以这里所有成员都能直接访问。
///         它没有构造函数、没有虚函数，是一个纯数据聚合体（aggregate）。
/// 【C 对照】就是 C 的 struct，只是成员里出现了 std::string / std::vector
///         这些「会自己管理内存」的类型 —— 它们让这个 struct 不能 memcpy。
struct JobOutcome {
    RunState state = RunState::SpawnError;
    std::string detail;          ///< 一句话交代怎么结束的
    long long duration_us = 0;
    std::size_t out_lines = 0;
    std::size_t err_lines = 0;
    std::size_t out_bytes = 0;
    std::size_t err_bytes = 0;
    bool truncated = false;
    std::vector<std::string> out_preview;
    std::vector<std::string> err_preview;
};

/// 执行一个作业时外部给它的约束。
struct JobContext {
    int timeout_ms = 5000;
    std::size_t max_preview_lines = 3;
    /// 指向全局停止标志。用户按 Ctrl-C 时它会变成 true。
    /// 【知识点：为什么用指针】这里只需要「看」它，不需要「拥有」它；
    /// 而且 std::atomic<bool> 不可拷贝，用引用或指针是唯一自然的选择。
    ///
    /// 【语法】const std::atomic<bool>* 是「指向 const atomic 的指针」：
    ///         通过它只能 load()，不能 store()，权限最小化。
    /// 【C 对照】C 里会写 volatile sig_atomic_t *stop；C++ 用 std::atomic
    ///         表达「多线程可见」，语义比 volatile 准确得多。
    const std::atomic<bool>* stop = nullptr;
};

/// 作业的抽象基类。
///
/// 【语法】class 默认 private，所以下面显式写了 public:。
/// 【规则】抽象类不能直接实例化，但可以有构造函数（供子类调用）。
class Job {
public:
    // 【知识点：虚析构函数】通过 Job* 删除一个 ShellJob 时，
    // 如果析构函数不是虚的，只会调用 Job 的析构 —— 子类成员泄漏。
    // 一条铁律：只要一个类打算被继承并通过基类指针删除，析构就必须是虚的。
    //
    // 【语法】= default 让编译器生成空实现；virtual 保证动态分派。
    //         noexcept 是隐式的（析构默认 noexcept）。
    virtual ~Job() = default;

    /// 作业名，只读。
    /// 【语法】= 0 是纯虚函数：Job 自己不实现，子类必须实现。
    ///         末尾的 const 表示不修改对象。
    [[nodiscard]] virtual std::string name() const = 0;

    /// 命令行展示形式（进程内作业返回 "<进程内> 名字"）。
    [[nodiscard]] virtual std::string command_line() const = 0;

    /// 执行并返回结果。注意：这是 const 成员函数 —— 作业对象本身不保存
    /// 运行状态，结果通过返回值交出去。这样同一个 Job 可以被重复执行。
    [[nodiscard]] virtual JobOutcome run(const JobContext& context) const = 0;

    /// 这个作业会不会真的开出子进程？用来在报告里区分两类作业。
    ///
    /// 【语法】noexcept 承诺不抛异常；子类覆盖时也只能更严格，不能更宽松。
    [[nodiscard]] virtual bool uses_process() const noexcept = 0;
};

/// 跑一条外部命令：fork + exec + 抓输出。
///
/// 【语法】final 表示不许再派生。它有两个好处：
///         1. 读代码的人知道「继承体系到此为止」；
///         2. 编译器在确定动态类型时可以做去虚化优化。
class ShellJob final : public Job {
public:
    /// 【语法】构造函数按值接收两个 std::string/vector，再用 std::move 搬进成员。
    ///         按值 + move 是「要把参数存起来」时的惯用法：
    ///         传左值 → 拷贝一次；传右值 → 移动一次。比写两个重载简单。
    ShellJob(std::string name, std::vector<std::string> argv);

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string command_line() const override;
    [[nodiscard]] JobOutcome run(const JobContext& context) const override;
    [[nodiscard]] bool uses_process() const noexcept override { return true; }

private:
    std::string name_;
    std::vector<std::string> argv_;
};

/// 在当前线程里直接跑一段逻辑，完全不开进程。
///
/// 现实里这类作业很常见：一次内部一致性检查、一次汇总计算，
/// 为它 fork 一个进程纯属浪费。把它做成 Job 的另一个子类，
/// 上层的线程池和报告逻辑一行都不用改。
class BuiltinJob final : public Job {
public:
    /// 返回它「打印」出来的若干行；抛异常即视为作业失败。
    ///
    /// 【语法】using 别名让签名只写一次。
    ///         这个类型等价于 std::function<std::vector<std::string>()>：
    ///         一个「无参数、返回 vector<string>」的可调用对象。
    using Body = std::function<std::vector<std::string>()>;

    BuiltinJob(std::string name, Body body);

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string command_line() const override;
    [[nodiscard]] JobOutcome run(const JobContext& context) const override;
    [[nodiscard]] bool uses_process() const noexcept override { return false; }

private:
    std::string name_;
    Body body_;
};

/// 构造这份练习的默认作业清单。
///
/// 这些作业是刻意挑的：每一条都在压一个具体的机制 ——
/// 大输出量压「必须持续抽管道」，自杀信号压「怎么区分退出码与终止信号」，
/// 长睡眠压「超时后必须整组杀掉」，不存在的二进制压「exec 成没成功」。
/// 全部只用 sh + coreutils，不依赖任何第三方程序。
///
/// 【语法】返回 vector<unique_ptr<Job>>：
///         - vector 可移动，适合返回一堆对象；
///         - unique_ptr 表达「唯一所有权」；
///         - 元素是基类指针，实际指向子类 —— 多态持有。
/// 【规则】析构这个 vector 时，每个 unique_ptr 会 delete 它指向的对象；
///         因为 Job 的析构是虚的，子类的析构会被正确调用。
std::vector<std::unique_ptr<Job>> make_default_jobs();

}  // namespace jobrun
