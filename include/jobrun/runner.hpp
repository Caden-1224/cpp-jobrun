// ============================================================================
//  runner.hpp —— 线程池：把一堆作业并发地跑完
//
// ============================================================================
//  【给只会 C 的你：线程池在 C 里要写什么】
//  ============================================================================
//
//  C + pthread 的版本大概长这样：
//
//      pthread_t workers[N];
//      pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
//      pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
//      struct job *queue;  int head, tail, count, closed;
//      void *worker(void *arg) {
//          for (;;) {
//              pthread_mutex_lock(&m);
//              while (count == 0 && !closed) pthread_cond_wait(&cv, &m);
//              if (count == 0 && closed) { pthread_mutex_unlock(&m); return NULL; }
//              struct job j = queue[head++]; count--;
//              pthread_mutex_unlock(&m);
//              run(&j);
//          }
//      }
//
//  C++ 把这一整套收进标准库：
//      std::thread         对应 pthread_create
//      std::mutex          对应 pthread_mutex_t
//      std::condition_variable 对应 pthread_cond_t
//      std::atomic<bool>   对应「加锁保护的 bool」或 C11 的 _Atomic
//  而且析构函数会自动 join/解锁，不会因为某个 return 忘了 pthread_join。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. 聚合体 + 默认成员初始化
//         struct RunnerConfig { std::size_t concurrency = 4; ... };
//     RunnerConfig config; 直接得到一个可用的默认配置。
//
//  2. std::atomic<bool>
//         std::atomic<bool> stop_{false};
//     多线程读写的 bool，必须用 atomic，否则是数据竞争（未定义行为）。
//     stop_.store(true) / stop_.load() 是原子操作；memory_order_relaxed
//     只保证「这个变量本身不被撕裂」，不提供跨变量的顺序保证。
//     对一个「只用来通知停止」的标志，relaxed 足够；如果你还依赖其它
//     普通变量先于标志可见，就需要 release/acquire。
//
//  3. 删除拷贝
//     std::atomic 不可拷贝，所以 Runner 也不可拷贝。
//     显式 = delete 让接口更清楚。
//
//  4. 按值传 vector<unique_ptr<Job>>
//         RunReport run(std::vector<std::unique_ptr<Job>> jobs);
//     调用方写 runner.run(std::move(jobs))，所有权明确交给 Runner。
//     传值 + move 语义：左值会拷贝（unique_ptr 不允许，所以编译失败），
//     右值会移动 —— 编译器帮你强制「交出所有权」。
//
//  5. noexcept 的简单 getter
//         void request_stop() noexcept { ... }
//     不抛异常的函数标 noexcept，调用方和标准库都能据此优化。
//
// ============================================================================
//  【扩展：线程还有哪些标准设施】
//  ============================================================================
//  * std::jthread（C++20）：析构自动 join，并带 stop_token 协作取消。
//    本项目手写 stop_ 标志，相当于自己实现了 stop_token 的核心思想。
//  * std::async / std::future / std::promise：任务级并发，拿返回值更方便，
//    但线程池控制力弱（不一定复用线程）。
//  * std::counting_semaphore / std::latch / std::barrier（C++20）：
//    计数、起跑线、阶段性同步。
//  * std::thread::hardware_concurrency()：拿 CPU 核数，用来决定默认并发度。
//  * pthread_setname_np：给线程起名，调试器里能看见（POSIX 扩展）。
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "jobrun/job.hpp"

namespace jobrun {

struct RunnerConfig {
    std::size_t concurrency = 4;    ///< worker 线程数
    std::size_t queue_capacity = 16;///< 待跑队列的最大长度（有界，形成背压）
    int timeout_ms = 1000;          ///< 单个作业的时限
    std::size_t preview_lines = 3;  ///< 每个作业保留几行输出样本
};

/// 一个作业在本次运行里的完整档案。
struct JobRecord {
    std::size_t sequence = 0;   ///< 提交序号，用来恢复「提交顺序」
    std::string name;
    std::string command;
    bool uses_process = false;
    JobOutcome outcome;
    long long queue_wait_us = 0;  ///< 排队等了多久
    long long wall_us = 0;        ///< 真正跑了多久
};

struct RunReport {
    std::vector<JobRecord> records;  ///< 已按提交序号排好
    std::size_t concurrency = 0;
    std::size_t submitted = 0;
    long long wall_us = 0;
    bool interrupted = false;        ///< 是否被 Ctrl-C 打断过
};

/// 作业执行器：N 个 worker 线程 + 一个信号观察线程。
///
/// 【知识点：为什么结果要按序号重排】作业是并发完成的，完成顺序每次都不一样。
/// 如果直接按完成顺序出报告，同一个程序跑两次得到两份不同的表格 —— 没法看，
/// 也没法对照。所以我们在收集时记下提交序号，最后排一次序：
/// 「过程不确定，结论确定」，这是并发程序可测试性的基本手法。
///
/// 【线程模型】
///     主线程：把作业 push 进有界队列，然后 close + join
///     worker × N：pop → 执行 → 记录结果
///     monitor × 1：poll 信号管道，收到 Ctrl-C 就置 stop_
/// 【生命周期】run() 返回前会 join 所有线程；Runner 析构时没有线程残留。
class Runner {
public:
    explicit Runner(RunnerConfig config);
    ~Runner() = default;

    /// 【语法】= delete 的拷贝：Runner 里有 atomic，本身也不可拷贝。
    ///         显式写出来，是为了给出清楚的编译错误。
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    /// 阻塞地把所有作业跑完，返回按提交顺序排好的报告。
    ///
    /// 【参数】jobs：按值接收，调用方用 std::move 交出所有权。
    /// 【返回】RunReport：值语义，调用方拥有结果，不涉及任何裸指针。
    RunReport run(std::vector<std::unique_ptr<Job>> jobs);

    /// 请求停止：正在跑的作业会在下一个 poll 分片里看到这个标志。
    /// 【语法】noexcept：这个操作只是原子写，不会失败。
    void request_stop() noexcept { stop_.store(true, std::memory_order_relaxed); }

    /// 查询是否已经请求停止。
    [[nodiscard]] bool stop_requested() const noexcept { return stop_.load(std::memory_order_relaxed); }

private:
    RunnerConfig config_;
    /// 【语法】_ 后缀只是命名习惯，表示「成员变量」。
    ///         {false} 是成员初始化：Runner 构造时 stop_ 就是 false。
    std::atomic<bool> stop_{false};
};

}  // namespace jobrun
