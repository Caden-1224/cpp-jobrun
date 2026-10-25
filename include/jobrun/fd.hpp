// ============================================================================
//  fd.hpp —— 用 RAII 管住一个文件描述符
//
// ============================================================================
//  【这个文件给「只会 C」的你】
// ============================================================================
//
//  C 的写法：
//      int fd = open(path, O_RDONLY | O_CLOEXEC);
//      if (fd < 0) { perror("open"); return -1; }
//      ...
//      close(fd);                      // 每个 return / goto 之前都得记得
//
//  C++ 的写法：
//      Fd fd = Fd::open_read(path);    // 失败会抛异常，不会安静地返回 -1
//      ...                             // 从任何路径离开作用域，析构都会 close
//
//  关键不是「少写一行 close」，而是「忘记 close」从运行期问题变成了
//  类型问题：Fd 离开作用域就必然析构，析构就必然 close。
//  这就是 RAII（Resource Acquisition Is Initialization）：
//      资源的获取 = 对象的构造，资源的释放 = 对象的析构。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
// ============================================================================
//
//  1. class / struct
//     C 的 struct 只能装数据；C++ 的 class 还能装函数（成员函数）、构造/析构、
//     继承、模板。struct 与 class 的唯一区别是默认访问权限：
//         struct 默认 public，class 默认 private。
//     实例化：Fd fd(3); 直接在栈上构造，不需要 malloc。
//     new Fd(3) 才是在堆上，而且必须 delete —— 这就是为什么这里用栈对象。
//
//  2. 访问控制
//         public:    谁都能访问
//         private:   只有本类的成员函数能访问
//         protected: 本类 + 派生类（本文件没用到）
//
//  3. 构造函数 / 析构函数
//     构造函数：与类同名、没有返回类型，对象创建时自动调用。
//     析构函数：~类名()，对象销毁时自动调用，不能重载、不能抛异常。
//     它们没有返回值，也不该被显式调用（placement new 等场景除外）。
//
//  4. explicit
//     禁止「一个参数的构造函数被当成隐式类型转换」。
//     没有 explicit 时，void f(Fd); f(3); 会悄悄构造一个 Fd(3)，
//     然后析构时 close(3) —— 关掉一个你根本没打算交给它的 fd。
//
//  5. noexcept
//     「这个函数不抛异常」的承诺。移动构造、移动赋值、析构必须写：
//     std::vector 扩容时会优先选择 noexcept 的移动构造；没有它就可能退化成
//     拷贝 —— 而 Fd 的拷贝已经被删除，于是直接编译失败。
//
//  6. = delete / = default
//         = delete  ：明确禁用某个函数（这里禁用了拷贝构造和拷贝赋值）
//         = default ：让编译器生成默认实现
//     两者都写在声明处，是「意图即文档」。
//
//  7. Rule of Five
//     一个类如果自己管理资源（fd、指针、锁），就要同时考虑五个函数：
//         析构、拷贝构造、拷贝赋值、移动构造、移动赋值
//     这里：析构自己写、拷贝删除、移动自己写 —— 因为两个 Fd 拥有同一个 fd
//     会导致 double close。
//
//  8. [[nodiscard]] / [[noreturn]]
//         [[nodiscard]]：返回值不许被忽略，否则编译器警告。
//         [[noreturn]] ：这个函数永不返回（通常因为它总是抛异常或退出）。
//
//  9. static 成员函数
//     不需要对象就能调用：Fd::make_pipe()，函数里没有 this 指针。
//     它和「命名空间里的自由函数」等价，只是名字被关进了类里。
//
// 10. 返回 std::pair / 结构化绑定
//     std::pair<Fd, Fd> 是「两个值打成一个包」。C 里你只能靠输出参数：
//         int make_pipe(int fds[2]);
//     C++ 里可以返回，调用方用 auto [r, w] = Fd::make_pipe(); 拆开。
//
// ============================================================================
//  【相关 POSIX API 速查（参数 + 返回 + 扩展）】
// ============================================================================
//
//  int open(const char *pathname, int flags, ... /* mode_t mode */);
//      pathname : 路径
//      flags    : O_RDONLY / O_WRONLY / O_RDWR 三选一，可再或上
//                 O_CREAT / O_TRUNC / O_APPEND / O_CLOEXEC / O_NONBLOCK / O_DIRECT
//      mode     : 仅当 flags 含 O_CREAT 时需要，例如 0644
//      返回     : 成功返回「当前最小的未用 fd」（非负）；失败返回 -1 并设置 errno
//      扩展     : openat(2)（相对 dirfd，避免 TOCTOU 竞态）、creat(2)、
//                 memfd_create(2)（内存文件）、O_TMPFILE（无名临时文件）
//
//  int close(int fd);
//      返回 0 成功，-1 失败。注意：Linux 上即使返回 EINTR，fd 也已经释放，
//      不要重试（详见 fd.cpp 的 reset()）。
//
//  int pipe2(int pipefd[2], int flags);
//      pipefd[0] 读端，pipefd[1] 写端
//      flags    : O_CLOEXEC / O_NONBLOCK（Linux 专有；macOS 只有 pipe()）
//      返回     : 0 成功，-1 失败
//      扩展     : pipe()、socketpair(2)（双向管道）、eventfd(2)（计数器事件）
//
//  int fcntl(int fd, int cmd, ...);
//      cmd      : F_GETFL / F_SETFL 读写「文件状态标志」（O_NONBLOCK 在这里）
//                 F_GETFD / F_SETFD 读写「文件描述符标志」（FD_CLOEXEC 在这里）
//                 F_DUPFD_CLOEXEC  复制 fd 并原子地设置 CLOEXEC
//      返回     : 取决于 cmd；失败 -1 + errno
// ============================================================================
#pragma once

// 【语法】#pragma once 不是标准 C++，但所有主流编译器都支持。
// 标准写法是 include guard：
//     #ifndef JOBRUN_FD_HPP
//     #define JOBRUN_FD_HPP
//     ...
//     #endif
// 两者作用相同：同一个编译单元里，这个头文件只被展开一次，避免重复定义。
// 【C 对照】C 里只能用 include guard；#pragma once 是编译器扩展，但已成事实标准。

#include <string>
#include <utility>

// 【语法】namespace 把名字关进一个作用域。
// 【C 对照】C 里靠前缀：jobrun_fd_make_pipe()；C++ 里靠 jobrun::make_pipe()，
//   编译器会检查你写的是哪个命名空间，也不会和别人的 make_pipe 撞名。
//   匿名 namespace（见 .cpp）则相当于给内部链接加一个「文件私有」的壳。
namespace jobrun {

/// 独占一个文件描述符的句柄：析构即 close，不可能忘记，也不可能泄漏。
///
/// 【语法】class 名字 { public: ... private: ... };
///   - 末尾的分号不能省（C 的 struct 也一样）
///   - 类体内直接定义的成员函数默认是 inline 的
///   - private 成员只能在成员函数里访问，外部只能通过 public 接口
///
/// 【C 对照】C 里你会写 struct Fd { int fd; }; 再配一对 fd_open/fd_close。
///   C++ 把「数据 + 操作它的函数」绑在一起，编译器还能保证：
///   外部代码碰不到 fd_，只能走 get()/release()/reset() 这些有明确语义的入口。
class Fd {
public:
    /// 默认构造：一个「不拥有任何 fd」的空句柄。
    ///
    /// 【语法】= default 让编译器生成默认实现。
    /// 【规则】noexcept 在这里是必须的：移动构造/移动赋值/析构都应是 noexcept，
    ///        否则标准容器在扩容时可能拒绝使用移动构造。
    /// 【实现】fd_ 的初始值来自下面的「类内成员初始化」：int fd_ = -1;
    Fd() noexcept = default;

    /// 接管一个已经打开的 fd。不检查合法性，调用方负全责。
    ///
    /// 【语法】explicit 阻止隐式转换：
    ///         void f(Fd);  f(3);        // 编译错误（好）
    ///         void f(Fd);  f(Fd(3));    // 显式构造（可以）
    /// 【规则】单个参数的构造函数默认都该加 explicit，除非你明确想要隐式转换。
    ///         Fd 这种「拥有资源」的类尤其不能允许隐式转换，否则一个 int
    ///         会在你毫无察觉时变成一个会 close 的句柄。
    explicit Fd(int fd) noexcept;

    /// 析构函数：对象离开作用域时自动调用 reset()，于是 fd 被 close。
    ///
    /// 【语法】~类名()，无参数、无返回值、不能重载。
    /// 【规则】析构函数不能抛异常（C++11 起隐式 noexcept）。
    ///         资源释放失败只能吞掉或记日志 —— 抛出去会直接 std::terminate。
    /// 【C 对照】相当于你手写的 fd_close()，但由编译器在作用域结束时自动调用，
    ///         包括 return、break、continue、异常展开这些路径。
    ~Fd();

    // 【知识点：Rule of Five】一个「拥有资源」的类，编译器默认生成的
    // 拷贝构造会按位复制那个 int —— 于是两个 Fd 拥有同一个 fd，
    // 析构时就会 close 两次（第二次关掉的可能已经是别人的 fd）。
    // 所以拷贝必须显式删掉，只保留移动：
    //
    // 【语法】= delete 是 C++11 的写法，等价于把函数声明为 private 且不实现
    //        （C++03 的土办法）。它参与重载决议，会在编译期直接报错，
    //        比「链接期找不到符号」清楚得多。
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    /// 移动构造：把 other 的 fd「搬」过来，other 变成空句柄。
    ///
    /// 【语法】Fd(Fd&& other) 里的 && 是「右值引用」，只绑定临时对象或
    ///         std::move(x) 这种明确表示「我不要了」的表达式。
    ///         std::move 本身不移动任何东西，它只是一个 static_cast<T&&>。
    /// 【规则】移动后 other 必须处于「可析构、可赋值」的合法状态（这里是 -1）。
    ///         不这么做，两个对象会同时 close 同一个 fd。
    /// 【C 对照】C 里传所有权靠约定：int take_fd(int *fd) { int x=*fd; *fd=-1; return x; }
    ///         C++ 把「交出去」变成类型系统里的一个动作。
    Fd(Fd&& other) noexcept;

    /// 移动赋值：先释放自己原来的 fd，再接管 other 的。
    ///
    /// 【语法】返回 Fd& 是为了支持链式赋值 a = b = c。
    /// 【坑】必须处理自赋值 a = std::move(a)：先 reset 会把 fd 关掉，
    ///      再 std::move 过来就是 -1。实现里用 if (this != &other) 挡住。
    Fd& operator=(Fd&& other) noexcept;

    /// 取原始 fd。只读，不转移所有权。
    ///
    /// 【语法】[[nodiscard]]：调用方必须使用返回值，否则编译器警告。
    ///         典型的系统调用包装都应该加，防止「调了但忘了看结果」。
    /// 【语法】const 成员函数：承诺不修改对象状态，因此 const Fd 也能调用。
    [[nodiscard]] int get() const noexcept;

    /// 是否持有一个合法的 fd（>= 0）。
    [[nodiscard]] bool valid() const noexcept;

    /// 让 Fd 能直接出现在 if 里：if (fd) { ... }
    ///
    /// 【语法】explicit operator bool() 是「显式布尔转换」。
    ///         没有 explicit 的话，Fd 会被隐式转成 int/bool，参与算术和比较，
    ///         几乎一定会写出 fd1 + fd2 这种鬼东西。
    /// 【C 对照】C 里写 if (fd >= 0)；C++ 里写成 if (fd) 更接近资源语义。
    explicit operator bool() const noexcept;

    /// 放弃所有权并把裸 fd 交出去；调用方从此负责 close。
    ///
    /// 【知识点：为什么必须有 release()】析构要 close，但有些场合
    /// 我们必须把 fd「传出去」（例如交给另一个需要裸 int 的接口）。
    /// 没有 release() 的话，就只能泄漏或者 double close。
    ///
    /// 【语法】= -1 之后再返回旧值，是「先取出、再置空」的经典写法。
    int release() noexcept;

    /// 关掉当前 fd（如果有），接管新 fd。传 -1 表示只关不接管。
    ///
    /// 【规则】默认参数只能写在声明处，不能重复写在定义处（见 fd.cpp）。
    /// 【规则】noexcept：析构会调用它，而析构不能抛异常。
    void reset(int fd = -1) noexcept;

    /// 打开一个只读文件，失败抛 std::system_error。
    ///
    /// 【语法】static 成员函数：不需要对象，用 Fd::open_read(path) 调用。
    /// 【语法】返回 Fd 而不是 Fd*：调用方拿到的是一个值对象，不是指针；
    ///         配合 C++17 的拷贝省略（guaranteed copy elision），连移动都省了。
    static Fd open_read(const std::string& path);

    /// 创建/截断一个文件用于写入（O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC）。
    static Fd create(const std::string& path);

    /// 返回 {读端, 写端}，两端都已置 O_CLOEXEC。
    ///
    /// 【语法】std::pair<Fd, Fd> 是标准库的二元组，用 .first / .second 访问，
    ///         或者用 C++17 的结构化绑定：auto [r, w] = Fd::make_pipe();
    /// 【C 对照】C 只能用输出参数 int fds[2]；返回 pair 让「两个返回值」变得自然。
    static std::pair<Fd, Fd> make_pipe();

    /// 打开 /dev/null 用于读写。
    static Fd dev_null();

private:
    // 【语法】类内成员初始化（NSDMI）：没有在构造函数里显式赋值的成员，
    //        会用它作为默认值。它让「默认构造 = 空句柄」这件事只写一次。
    // 【C 对照】C 的 struct 没有成员默认值，只能靠 memset 或初始化器。
    int fd_ = -1;
};

/// 打开/关闭 O_NONBLOCK。
/// 【为什么是自由函数而不是成员】这个操作也适用于裸 int fd（例如子进程里
/// 刚 dup2 出来的 fd），不只是 Fd 对象。放在命名空间里更通用。
void set_nonblocking(int fd, bool enable);

/// 打开/关闭 O_CLOEXEC（fork 出的子进程 exec 时自动关掉这些 fd）。
void set_cloexec(int fd, bool enable);

/// 用当前 errno 抛出 std::system_error。
///
/// 【语法】[[noreturn]] 告诉编译器「这个函数不会正常返回」，于是
///         它后面的代码不会被当作「可到达」而报「缺少 return」警告。
/// 【语法】const std::string& 是「只读引用」：不拷贝字符串，也允许传字面量
///         （字面量会隐式构造一个临时 std::string 绑定到 const 引用上）。
[[noreturn]] void fail(const std::string& what);

}  // namespace jobrun
