// ============================================================================
//  queue.hpp —— 线程安全的有界队列
//
// ============================================================================
//  【这个文件教什么】
// ============================================================================
//
//  一句话：把操作系统课上的「有界缓冲区 / 生产者消费者」用 C++ 写一遍。
//
//  ============================================================================
//  【给只会 C 的你：模板是什么】
//  ============================================================================
//
//  C 里想写一个「什么元素都能装」的队列，只有三条路：
//      1. 存 void*：丢失类型，调用方必须自己记住「这里装的是 Job* 还是 int」；
//         还要额外传一个 sizeof，取出来时手动转型。
//      2. 存字节数组 + memcpy：更通用，但完全没有类型检查。
//      3. 写宏：能展开，但调试困难和代码膨胀都是老问题。
//
//  C++ 的模板是「编译期生成代码」：
//      template <typename T>
//      class BoundedQueue { ... };
//      BoundedQueue<int>  q1(8);        // 编译器生成一份 BoundedQueue<int>
//      BoundedQueue<Job*> q2(8);        // 再生成一份 BoundedQueue<Job*>
//
//  它和 C 的宏有本质区别：
//      * 模板是类型安全的：T 是 int 时，push("abc") 直接编译失败；
//      * 模板参与重载决议、能做特化、能被 IDE 理解；
//      * 代价是「每种 T 一份机器码」—— 这就是所谓的「模板膨胀」，
//        所以模板实现只能放在头文件里（见下）。
//
//  ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. 模板声明
//         template <typename T>
//         class BoundedQueue { ... };
//     typename 和 class 在这里等价；现代写法倾向 typename（T 不一定是类）。
//
//  2. 为什么实现必须写在头文件里
//     编译器在看到 BoundedQueue<int> 的那一刻，才需要生成这份代码。
//     如果实现放在 queue.cpp，而使用处在另一个 .cpp，编译器看不到函数体，
//     只能生成一个「外部符号引用」，链接时却找不到 —— 经典 undefined reference。
//     所以模板（以及 inline 函数、constexpr 函数）的实现通常放头文件。
//
//  3. explicit 构造函数
//         explicit BoundedQueue(std::size_t capacity);
//     防止 BoundedQueue<int> q = 8; 这种隐式转换。
//
//  4. 删除拷贝
//         BoundedQueue(const BoundedQueue&) = delete;
//     成员里有 std::mutex，而 mutex 本身不可拷贝，所以这个类天生不可拷贝。
//     显式写出来，是为了让「不能拷贝」成为接口的一部分，而不是等编译器报
//     一长串模板错误。
//
//  5. 成员函数在类体内定义
//     类体内定义的成员函数默认是 inline 的（每个翻译单元都能看到同一份定义，
//     链接器负责去重）。对模板来说这是必须的。
//
//  6. std::unique_lock 与 std::lock_guard
//         lock_guard : 构造即 lock、析构即 unlock，不可移动、不可提前解锁。
//         unique_lock: 可移动、可 unlock()/lock() 手动控制、能配合 condition_variable。
//     condition_variable::wait 需要在睡眠时解锁、醒来时重新加锁，
//     lock_guard 做不到，所以这里必须用 unique_lock。
//
//  7. mutable
//         mutable std::mutex mutex_;
//     size() 是 const 成员函数，但加锁会修改 mutex 的内部状态。
//     mutable 告诉编译器：「这个成员在 const 函数里也可以改」。
//     它只该用在「逻辑上不改变对象值」的同步设施上，别拿它绕过 const 正确性。
//
//  8. [[nodiscard]]
//     返回值不许被忽略。size()/capacity()/closed() 被忽略通常意味着逻辑错误。
//
//  ============================================================================
//  【本文件涉及的 C++ 标准库 API：参数与语义】
//  ============================================================================
//
//  std::mutex
//      lock() / unlock() / try_lock()
//      【规则】谁 lock 谁 unlock；异常路径要靠 lock_guard/unique_lock 兜底。
//
//  std::lock_guard<std::mutex> guard(m);
//      构造时 m.lock()，析构时 m.unlock()。不可拷贝、不可移动。
//      适合「整个作用域都持锁」的简单场景。
//
//  std::unique_lock<std::mutex> lock(m);
//      构造时 lock，析构时 unlock（若仍持有）。可移动、可 unlock 后再 lock。
//      额外构造参数：
//          std::defer_lock  先不锁，之后手动 lock()
//          std::try_to_lock 尝试锁，不阻塞
//          std::adopt_lock   接管一个已经锁住的 mutex
//
//  std::condition_variable
//      wait(unique_lock&, pred)  睡眠直到 pred() 为真（谓词版本，必须用）
//      wait_for / wait_until     带超时的版本
//      notify_one()              唤醒一个等待者
//      notify_all()              唤醒全部等待者
//      【规则】wait 必须配 unique_lock；谓词版本自己处理虚假唤醒。
//
//  std::deque<T>
//      双端队列。push_back/pop_front 都是 O(1)，扩容不搬移已有元素。
//      队列只需要头尾操作，deque 比 vector 更合适（vector 头删是 O(n)）。
//
//  std::optional<T>
//      「可能有值，也可能没有」的类型。C 里通常用「返回 -1 / NULL / 输出参数
//      + bool 返回值」表达；optional 把「有没有」和「值是什么」合成一个类型，
//      调用方无法忘记检查。
//      API：has_value()、operator bool、value()（没有值会抛 bad_optional_access）、
//           value_or(default)、operator*、operator->。
//
//  ============================================================================
//  【扩展：还可以怎么实现】
//  ============================================================================
//  * C++20 的 std::counting_semaphore / std::latch / std::barrier：
//    有界队列的「容量计数」可以用信号量表达，代码更短，但语义不如条件变量直观。
//  * 无锁环形缓冲区：用 std::atomic<size_t> 的 head/tail + acquire/release，
//    单生产者单消费者可以做到无锁；多生产者需要更复杂的算法（CAS 循环）。
//  * std::queue<T> + std::mutex：标准库现成的容器适配器，但没有「有界」和
//    「关闭」这两个语义，需要自己再包一层。
//  * 如果 T 很大，pop 时用 std::move 把元素搬出来，避免拷贝。
// ============================================================================
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace jobrun {

/// 有界阻塞队列：满则 push 阻塞，空则 pop 阻塞，close 唤醒所有人。
///
/// 它就是操作系统课上的「有界缓冲区」——生产者消费者问题的那个缓冲区。
///
/// 【模板参数】T 是元素类型。这里实际用的是 PendingJob（见 runner.cpp），
///             它内部有 std::unique_ptr<Job>，所以只能移动、不能拷贝。
///             队列的 push(T value) 和 pop() 都按值 + std::move 处理，
///             对只能移动的类型同样成立。
template <typename T>
class BoundedQueue {
public:
    /// 构造：容量为 0 时按 1 处理，避免「永远 push 不进去」的死锁。
    ///
    /// 【语法】explicit 阻止 BoundedQueue<int> q = 16; 这种隐式转换。
    /// 【语法】: capacity_(...) 是成员初始化列表；capacity_ 是 const 吗？
    ///         不是，但它在构造后再也不变，逻辑上是只读的。
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    // 【知识点：删除拷贝】内部有 mutex，mutex 不可拷贝，
    // 所以这个类天生不可拷贝。此处显式写出来，是为了让意图清晰。
    //
    // 【语法】= delete 的函数仍然参与重载决议：如果你写
    //         BoundedQueue<int> b = a; 编译器会明确告诉你「拷贝已被删除」，
    //         而不是给你一屏模板实例化错误。
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    /// 入队。队列已关闭时返回 false（调用方应据此停止生产）。
    ///
    /// 【参数】value 按值传入。对只能移动的类型（如 PendingJob），
    ///         调用方要写 queue.push(std::move(x))；这里再用 std::move
    ///         把它搬进 deque，全程不发生拷贝。
    ///
    /// 【返回值】true = 入队成功；false = 队列已关闭，调用方应停止生产。
    bool push(T value) {
        // 【知识点：unique_lock vs lock_guard】lock_guard 只在构造时锁一次，
        // 没法配合 condition_variable 的 wait（wait 会在睡眠时解锁、醒来时重新加锁），
        // 所以这里必须用 unique_lock。
        std::unique_lock<std::mutex> lock(mutex_);

        // 【知识点：带谓词的 wait】必须用谓词版本，绝不能用裸 wait()。
        // 原因有两个：一是虚假唤醒（spurious wakeup）随时可能发生；
        // 二是 notify 可能发生在你重新拿锁之前，那个信号就丢了。
        // 谓词版本会把「条件是否成立」重新判断一次，两种情况都挡住了。
        //
        // 这里的谓词：队列没满，或者队列已经关闭（关闭时 push 要立刻失败，
        // 不能继续睡在「没满」上）。
        not_full_.wait(lock, [this] { return items_.size() < capacity_ || closed_; });
        if (closed_) {
            return false;
        }
        // 【语法】std::move(value) 把左值转成右值引用，触发 T 的移动构造。
        //         对 std::string 这类类型，移动通常只是搬指针，不拷贝内容。
        items_.push_back(std::move(value));
        lock.unlock();  // 【知识点：缩小临界区】先解锁再通知，避免被唤醒的线程立刻又阻塞在锁上
        not_empty_.notify_one();
        return true;
    }

    /// 出队。队列已关闭且为空时返回 nullopt。
    ///
    /// 【返回值】std::optional<T>：
    ///         - 有值   → 取到了一个元素
    ///         - nullopt → 队列已关闭且已排空，消费者应当退出
    ///         C 里通常用 int pop(T *out) 返回 0/1；optional 让调用方不可能
    ///         忘记检查「到底有没有取到」。
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        // 【知识点：带谓词的 wait】必须用谓词版本，绝不能用裸 wait()。
        // 原因有两个：一是虚假唤醒（spurious wakeup）随时可能发生；
        // 二是 notify 可能发生在你重新拿锁之前，那个信号就丢了。
        // 谓词版本会把「条件是否成立」重新判断一次，两种情况都挡住了。
        not_empty_.wait(lock, [this] { return !items_.empty() || closed_; });
        if (items_.empty()) {
            // 关闭 + 空 = 消费者该退出了。
            return std::nullopt;
        }
        // 【语法】先 move 再 pop_front：如果先 pop_front，元素已经被销毁，
        //         就没东西可 move 了。move 之后 items_.front() 处于「有效但
        //         未指定」状态，紧接着 pop_front 销毁它，安全。
        T value = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    /// 关闭队列：唤醒所有等待者，之后 push 一律失败、pop 排空后返回 nullopt。
    ///
    /// 【知识点：为什么用 notify_all 而不是 notify_one】关闭是「所有消费者
    /// 都该醒」的事件。如果只唤醒一个，其余消费者会永远睡在 not_empty_ 上，
    /// join() 就会永久挂住 —— 这是「程序退不出去」最常见的原因。
    void close() {
        {
            // 【语法】花括号制造一个局部作用域：lock 在离开这个作用域时析构、
            //         自动解锁。这样 notify 就在锁外进行，减少一次无谓的争抢。
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        // 【规则】notify 可以在锁外调用。被唤醒的线程会重新尝试拿锁，
        //         只要它检查谓词（closed_），就不会丢事件。
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /// 当前元素个数（加锁读取）。
    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    /// 容量（构造后不变，所以不用加锁）。
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

    /// 队列是否已关闭。
    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    // 【知识点：mutable】size() 是 const 成员函数，却要加锁改 mutex_ 的状态，
    // 所以 mutex_ 必须声明为 mutable。
    mutable std::mutex mutex_;
    // 【知识点：两个条件变量】「非空」和「非满」是两个不同的等待条件。
    // 共用一个的话，notify_all 会把不相关的等待者也吵醒，白白空转。
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> items_;
    std::size_t capacity_;
    bool closed_ = false;
};

}  // namespace jobrun
