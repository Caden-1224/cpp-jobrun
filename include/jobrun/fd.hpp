// ============================================================================
//  fd.hpp —— 用 RAII 管住一个文件描述符
//
//  【知识点：头文件与分离编译】头文件只放「有什么」（声明），实现放 .cpp，
//  这样别的 .cpp 能看见接口，而实现只被编译一次、只在链接时被引用。
//  【知识点：#pragma once】防止同一头文件在一次编译单元里被包含两次。
//  【知识点：命名空间】把库里的名字关进 jobrun，避免和别人的 Fd 撞名。
// ============================================================================
#pragma once

#include <string>
#include <utility>

namespace jobrun {

/// 独占一个文件描述符的句柄：析构即 close，不可能忘记，也不可能泄漏。
///
/// 【知识点：RAII】Resource Acquisition Is Initialization ——
/// 把「资源的生命周期」绑定到「对象的生命周期」上。
/// 这是 C++ 与 C 最本质的分界：C 里你必须记得在每个 return 前 close，
/// C++ 里你只要保证对象会析构，剩下的事编译器替你办。
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept;
    ~Fd();

    // 【知识点：Rule of Five】一个「拥有资源」的类，编译器默认生成的
    // 拷贝构造会按位复制那个 int —— 于是两个 Fd 拥有同一个 fd，
    // 析构时就会 close 两次（第二次关掉的可能已经是别人的 fd）。
    // 所以拷贝必须显式删掉，只保留移动：
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    explicit operator bool() const noexcept;

    /// 放弃所有权并把裸 fd 交出去；调用方从此负责 close。
    ///
    /// 【知识点：为什么必须有 release()】析构要 close，但有些场合
    /// 我们必须把 fd「传出去」（例如交给另一个需要裸 int 的接口）。
    /// 没有 release() 的话，就只能泄漏或者 double close。
    int release() noexcept;

    /// 关掉当前 fd（如果有），接管新 fd。传 -1 表示只关不接管。
    void reset(int fd = -1) noexcept;

    static Fd open_read(const std::string& path);
    static Fd create(const std::string& path);
    /// 返回 {读端, 写端}，两端都已置 O_CLOEXEC。
    static std::pair<Fd, Fd> make_pipe();
    static Fd dev_null();

private:
    int fd_ = -1;  // 【知识点：类内初始化】成员默认值写在声明处，构造函数就不必操心
};

/// 打开/关闭 O_NONBLOCK。
void set_nonblocking(int fd, bool enable);
/// 打开/关闭 O_CLOEXEC（fork 出的子进程 exec 时自动关掉这些 fd）。
void set_cloexec(int fd, bool enable);

/// 用当前 errno 抛出 std::system_error。
[[noreturn]] void fail(const std::string& what);

}  // namespace jobrun
