// ============================================================================
//  queue.hpp —— 线程安全的有界队列
//
//  【知识点：模板】BoundedQueue<T> 对元素类型一无所知，T 由使用处决定。
//  和 Rust 的泛型不同，C++ 模板在编译期为每种 T 各生成一份代码，
//  所以实现必须写在头文件里 —— 编译器得看得到它才能实例化。
//  【知识点：头文件即实现】这是模板与普通类在工程组织上的最大区别。
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
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    // 【知识点：删除拷贝】内部有 mutex，mutex 不可拷贝，
    // 所以这个类天生不可拷贝。此处显式写出来，是为了让意图清晰。
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    /// 入队。队列已关闭时返回 false（调用方应据此停止生产）。
    bool push(T value) {
        // 【知识点：unique_lock vs lock_guard】lock_guard 只在构造时锁一次，
        // 没法配合 condition_variable 的 wait（wait 会在睡眠时解锁、醒来时重新加锁），
        // 所以这里必须用 unique_lock。
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return items_.size() < capacity_ || closed_; });
        if (closed_) {
            return false;
        }
        items_.push_back(std::move(value));
        lock.unlock();  // 【知识点：缩小临界区】先解锁再通知，避免被唤醒的线程立刻又阻塞在锁上
        not_empty_.notify_one();
        return true;
    }

    /// 出队。队列已关闭且为空时返回 nullopt。
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        // 【知识点：带谓词的 wait】必须用谓词版本，绝不能用裸 wait()。
        // 原因有两个：一是虚假唤醒（spurious wakeup）随时可能发生；
        // 二是 notify 可能发生在你重新拿锁之前，那个信号就丢了。
        // 谓词版本会把「条件是否成立」重新判断一次，两种情况都挡住了。
        not_empty_.wait(lock, [this] { return !items_.empty() || closed_; });
        if (items_.empty()) {
            return std::nullopt;
        }
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
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] std::size_t capacity() const { return capacity_; }

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
