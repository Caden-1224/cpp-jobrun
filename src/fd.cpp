// ============================================================================
//  fd.cpp —— Fd 的实现
//
//  【知识点：编译单元】这个文件加上它包含的头文件，就是一个 translation unit。
//  它只 #include 自己的头文件 + 需要的系统头文件，不去管别的 .cpp 长什么样。
// ============================================================================
#include "jobrun/fd.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace jobrun {

Fd::Fd(int fd) noexcept : fd_(fd) {}

Fd::~Fd() { reset(); }

Fd::Fd(Fd&& other) noexcept : fd_(other.release()) {}

Fd& Fd::operator=(Fd&& other) noexcept {
    // 【知识点：自赋值检查】a = std::move(a) 必须先判掉，
    // 否则 reset() 会先关掉 fd，再从这个已经为 -1 的 other 里取值。
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int Fd::get() const noexcept { return fd_; }

bool Fd::valid() const noexcept { return fd_ >= 0; }

Fd::operator bool() const noexcept { return valid(); }

int Fd::release() noexcept {
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
        ::close(fd_);
    }
    fd_ = fd;
}

Fd Fd::open_read(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fail("open(" + path + ")");
    }
    return Fd(fd);
}

Fd Fd::create(const std::string& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fail("create(" + path + ")");
    }
    return Fd(fd);
}

std::pair<Fd, Fd> Fd::make_pipe() {
    int fds[2] = {-1, -1};
    // 【知识点：原子地设置 CLOEXEC】pipe2 让「创建管道」和「设置 O_CLOEXEC」
    // 一步完成。如果先 pipe() 再 fcntl()，在多线程程序里中间那一小段窗口
    // 里恰好有别的线程 fork + exec，子进程就会白白继承这两个 fd。
    if (::pipe2(fds, O_CLOEXEC) < 0) {
        fail("pipe2");
    }
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
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        fail("fcntl(F_GETFL)");
    }
    const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(fd, F_SETFL, updated) < 0) {
        fail("fcntl(F_SETFL)");
    }
}

void set_cloexec(int fd, bool enable) {
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
    const int err = errno;
    throw std::system_error(err, std::generic_category(), what);
}

}  // namespace jobrun
