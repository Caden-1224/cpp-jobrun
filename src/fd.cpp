// ============================================================================
//  fd.cpp —— Fd 的实现
//
// ============================================================================
//  【这个文件给「只会 C」的你】
// ============================================================================
//
//  头文件（.hpp）说「有什么」，源文件（.cpp）说「怎么做」。
//  把 fd.cpp 单独编译，得到一个目标文件 fd.o；链接器再把所有 .o 拼起来。
//  C 里你也可以只写一个 .c，但一旦程序变大，就必须回答：
//      * 哪些函数是给别人用的（放头文件）
//      * 哪些是文件私有的（static 或匿名 namespace）
//      * 谁负责初始化、谁负责清理
//  C++ 用类把这三件事收进同一个文件对里。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
// ============================================================================
//
//  1. 翻译单元（translation unit）
//     一个 .cpp + 它包含的所有头文件 = 一个翻译单元。编译器一次只看一个，
//     看不到别的 .cpp 里有什么。链接器才负责把符号拼起来。
//
//  2. 成员初始化列表
//         Fd::Fd(int fd) noexcept : fd_(fd) {}
//     冒号后面的部分在「函数体执行之前」完成成员初始化。
//     【规则】成员按「声明顺序」初始化，不是按初始化列表里的书写顺序。
//            所以列表顺序最好和声明顺序一致，否则 -Wreorder 会警告。
//     【C 对照】C 里只能 struct Fd f = { fd }; 或者 f.fd = fd;（先构造再赋值）。
//              C++ 的初始化列表是「构造」，不是「赋值」，对 const 成员和引用成员
//              是唯一可行的方式。
//
//  3. 右值引用与移动
//         Fd(Fd&& other) noexcept : fd_(other.release()) {}
//     && 只绑定「临时对象」或 std::move(x)。std::move 只是把左值转成右值引用，
//     真正的「搬走」发生在被调用的函数体里（这里就是 release()）。
//
//  4. 自赋值
//         if (this != &other) { ... }
//     this 是指向当前对象的指针。a = std::move(a) 时 this == &other。
//     不判的话，reset() 会先关掉 fd，再从未定义的 other 里取值。
//
//  5. 运算符重载
//         Fd::operator bool() const noexcept { return valid(); }
//     让自定义类型支持 if (fd) 这种写法。explicit 版本只在「需要 bool」的
//     上下文里生效（if、while、!、&&、||），不会参与算术运算。
//
//  6. 静态成员函数的定义
//         不需要写 static 关键字（声明处已经写了），但必须写 Fd::
//
//  7. 异常
//     throw std::system_error(...) 会沿调用栈向上展开，沿途的栈对象
//     （包括 Fd）都会被析构 —— 这就是为什么 RAII 能保证不泄漏。
//
// ============================================================================
//  【本文件涉及的 POSIX API：参数、返回、errno、扩展】
// ============================================================================
//
//  int open(const char *pathname, int flags, ... /* mode_t mode */);
//      flags  : O_RDONLY / O_WRONLY / O_RDWR，可或上
//               O_CREAT（需要 mode）、O_TRUNC、O_APPEND、O_CLOEXEC、O_NONBLOCK
//      mode   : 新文件的权限位，受 umask 影响；0644 = rw-r--r--
//      返回   : 成功 = 最小可用 fd；失败 = -1，errno 说明原因
//      errno  : ENOENT（不存在）、EACCES（权限）、EMFILE（进程 fd 用完）、
//               ENFILE（系统 fd 用完）、EISDIR（对目录 O_WRONLY）
//
//  int close(int fd);
//      返回 0 / -1。Linux 上 close 返回 EINTR 时 fd 已经释放，不要重试。
//
//  int pipe2(int pipefd[2], int flags);
//      返回 0 / -1。pipefd[0] 只能读，pipefd[1] 只能写。
//      flags = O_CLOEXEC 让两端在 exec 时自动关闭；O_NONBLOCK 让读写不阻塞。
//      【为什么用 pipe2 而不是 pipe + fcntl】后者有竞态窗口：两个调用之间
//      如果别的线程 fork + exec，子进程会继承这两个还没设 CLOEXEC 的 fd。
//
//  int fcntl(int fd, int cmd, ...);
//      F_GETFL / F_SETFL：读/写「文件状态标志」，O_NONBLOCK 属于这一组，
//          它作用在「打开的文件描述」上 —— dup 或 fork 出来的 fd 共享同一份，
//          改一个，全都变。
//      F_GETFD / F_SETFD：读/写「描述符标志」，FD_CLOEXEC 属于这一组，
//          它作用在「单个 fd 槽位」上 —— dup 出来的新 fd 默认不带 CLOEXEC。
//      F_DUPFD_CLOEXEC：复制 fd 并原子地设置 CLOEXEC。
//
//  【扩展】openat(2) 用 dirfd 相对路径，避免「检查路径 → 再打开」之间被
//  替换的 TOCTOU 竞态；close_range(2) 一次关掉一段 fd；memfd_create(2)
//  创建匿名内存文件；eventfd(2) 做一个可 poll 的计数器。
// ============================================================================
#include "jobrun/fd.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace jobrun {

// 【语法】成员初始化列表写在冒号后面。fd_ 是 private 成员，外部看不到，
//        但成员函数（包括构造函数）可以访问。
Fd::Fd(int fd) noexcept : fd_(fd) {}

// 【语法】析构函数没有参数、没有返回类型。这里把清理逻辑委托给 reset()，
//        保证「析构」和「显式 reset」走同一条代码路径。
Fd::~Fd() { reset(); }

// 【语法】other.release() 在初始化列表里调用：先取走 fd，再把 other 置空。
//        移动构造结束后 other 是「有效但空」的状态，可以安全析构。
Fd::Fd(Fd&& other) noexcept : fd_(other.release()) {}

Fd& Fd::operator=(Fd&& other) noexcept {
    // 【知识点：自赋值检查】a = std::move(a) 必须先判掉，
    // 否则 reset() 会先关掉 fd，再从这个已经为 -1 的 other 里取值。
    if (this != &other) {
        // 【规则】先释放自己原来的资源，再接管对方的。
        // 顺序反了会泄漏：直接把 other 的 fd 赋给 fd_，旧 fd 就永远没人关了。
        reset(other.release());
    }
    // 【语法】返回 *this 的引用，是为了支持链式赋值 (a = b) = c。
    return *this;
}

int Fd::get() const noexcept { return fd_; }

bool Fd::valid() const noexcept { return fd_ >= 0; }

Fd::operator bool() const noexcept { return valid(); }

int Fd::release() noexcept {
    // 【语法】也可以写成 return std::exchange(fd_, -1);（<utility>，C++14）。
    //        这里手写三步，是为了让你看清「取出旧值 → 置空 → 返回」的顺序。
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void Fd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        // 【知识点：析构函数绝不抛异常】close() 会失败（例如 EINTR），
        // 但析构函数是 noexcept 的，异常从析构里逃出去会直接 terminate()。
        //
        // 【知识点：为什么这里不重试 EINTR】在 Linux 上，close() 即使返回
        // EINTR，fd 也已经被释放了。如果照着「EINTR 就重试」的老习惯再关一次，
        // 关掉的很可能是另一个线程刚打开的同号 fd —— 这是真实项目里查很久的 bug。
        // 所以正确做法是：忽略返回值，绝不重试。
        //
        // 【扩展】如果确实需要确认「关掉了没有」，用 close_range 或
        //        /proc/self/fd 检查；但不要用「再 close 一次」来确认。
        ::close(fd_);
    }
    fd_ = fd;
}

Fd Fd::open_read(const std::string& path) {
    // 【语法】std::string::c_str() 返回以 '\0' 结尾的 const char*，
    //        可以直接传给 C API。生命周期到下一次修改 string 为止。
    //        C++17 还有 data()（非 const 版本可写），但这里用 c_str() 最清楚。
    //
    // 【API】O_CLOEXEC：这个 fd 在执行 exec 时自动关闭。
    //        不设的话，子进程继承到的 fd 会一直开着，可能导致
    //        「管道写端永远不关 → read 永远等不到 EOF」。
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        // 【语法】fail() 是 [[noreturn]]，所以这里不需要 return。
        fail("open(" + path + ")");
    }
    // 【语法】返回 Fd(fd)：C++17 的「保证拷贝省略」让这个返回值直接
    //        在调用方构造，不会多一次移动。C++11/14 里则依赖 NRVO/移动。
    return Fd(fd);
}

Fd Fd::create(const std::string& path) {
    // 【API】O_CREAT 需要第三个参数 mode；这里 0644 表示
    //        所有者 rw-，组 r--，其他人 r--。实际权限还会 & ~umask。
    //        O_TRUNC：文件已存在则截断为 0 字节。
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fail("create(" + path + ")");
    }
    return Fd(fd);
}

std::pair<Fd, Fd> Fd::make_pipe() {
    // 【语法】int fds[2] = {-1, -1}; 是 C 风格的栈数组。
    //        C++ 里也可以写 std::array<int, 2> fds{};，但传给 C API 时要 .data()。
    //        【坑】C99 的变长数组（int a[n]）不是标准 C++，不要用。
    int fds[2] = {-1, -1};
    // 【知识点：原子地设置 CLOEXEC】pipe2 让「创建管道」和「设置 O_CLOEXEC」
    // 一步完成。如果先 pipe() 再 fcntl()，在多线程程序里中间那一小段窗口
    // 里恰好有别的线程 fork + exec，子进程就会白白继承这两个 fd。
    if (::pipe2(fds, O_CLOEXEC) < 0) {
        fail("pipe2");
    }
    // 【语法】返回 {Fd(fds[0]), Fd(fds[1])}：std::pair 的列表初始化。
    //        两个 Fd 各自接管一个 fd；如果第二个 Fd 构造时抛异常（这里不会），
    //        第一个 Fd 会被自动析构，不会泄漏。
    return {Fd(fds[0]), Fd(fds[1])};
}

Fd Fd::dev_null() {
    const int fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fail("open(/dev/null)");
    }
    return Fd(fd);
}

void set_nonblocking(int fd, bool enable) {
    // 【API】F_GETFL 读「文件状态标志」。必须「读-改-写」，不能直接 F_SETFL(O_NONBLOCK)，
    //        否则会把 O_APPEND、O_ASYNC 之类的标志一并抹掉。
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        fail("fcntl(F_GETFL)");
    }
    // 【语法】? : 三元运算符；~O_NONBLOCK 是位取反。
    const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(fd, F_SETFL, updated) < 0) {
        fail("fcntl(F_SETFL)");
    }
}

void set_cloexec(int fd, bool enable) {
    // 【API】F_GETFD / F_SETFD 操作的是「描述符标志」（目前只有 FD_CLOEXEC
    //        和 FD_CLOFORK），和 F_GETFL/F_SETFL 的「状态标志」不是一回事。
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        fail("fcntl(F_GETFD)");
    }
    const int updated = enable ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
    if (::fcntl(fd, F_SETFD, updated) < 0) {
        fail("fcntl(F_SETFD)");
    }
}

void fail(const std::string& what) {
    // 【知识点：错误与异常】把 errno 在抛之前抓下来 —— 构造 system_error 的
    // 过程本身可能调用别的函数、覆盖 errno。
    //
    // 【API】std::system_error(int ev, const std::error_category& cat, const std::string& what);
    //        ev   : 错误码，这里就是 errno
    //        cat  : generic_category() 对应 POSIX errno；system_category()
    //               对应操作系统原生错误码。在 Linux 上两者映射相同，
    //               但「errno 用 generic」是更准确的表达。
    //        what : 出错时做的事情，what() 会拼成 "open(x): No such file or directory"
    //
    // 【语法】throw 一个临时对象；catch (const std::exception&) 可以接住它，
    //        因为 system_error 继承自 runtime_error → exception。
    const int err = errno;
    throw std::system_error(err, std::generic_category(), what);

    // 【扩展】不想用异常的话，标准库还有 std::error_code：
    //        std::error_code ec(err, std::generic_category());
    //        if (ec) { ... ec.message() ... }
    //        C++23 的 std::expected<T, E> 则把「返回值 + 错误码」合成一个类型。
}

}  // namespace jobrun
