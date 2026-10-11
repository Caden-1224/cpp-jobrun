// ============================================================================
//  signals.cpp —— 自管道技巧的实现
// ============================================================================
#include "jobrun/signals.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <string>

namespace jobrun {
namespace {

/// 处理器唯一允许访问的全局状态。
///
/// 【知识点：为什么这里不需要加锁或 atomic】它在安装处理器之前写入，
/// 在卸载处理器之后清空，处理器运行期间它保持不变。既然没有并发写，
/// 也就不存在数据竞争。
int g_signal_write_fd = -1;

extern "C" void jobrun_signal_handler(int sig) {
    // 【知识点：保护 errno】处理器打断了正常代码，而正常代码可能正要读 errno。
    // 我们的 write() 会改写 errno，所以进来先存、出去再恢复。
    const int saved_errno = errno;
    const unsigned char byte = static_cast<unsigned char>(sig);
    if (g_signal_write_fd >= 0) {
        // 写端是非阻塞的：万一管道满了，write 立刻返回 EAGAIN，
        // 处理器绝不会在这里睡过去。（在这里阻塞等于把整个进程冻住。）
        const ssize_t ignored = ::write(g_signal_write_fd, &byte, 1);
        (void)ignored;
    }
    errno = saved_errno;
}

}  // namespace

std::string signal_name(int sig) {
    switch (sig) {
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGHUP: return "SIGHUP";
        case SIGKILL: return "SIGKILL";
        case SIGSEGV: return "SIGSEGV";
        case SIGPIPE: return "SIGPIPE";
        default: return "信号 " + std::to_string(sig);
    }
}

SignalWatcher::SignalWatcher() {
    // 【知识点：复用我们自己的 Fd】管道两端都带上 O_CLOEXEC，
    // 这样任何 fork 出来的子进程在 exec 之后都不会继承到这两个 fd。
    // O_NONBLOCK 则保证处理器的 write 永不阻塞。
    auto [read_end, write_end] = Fd::make_pipe();
    set_nonblocking(write_end.get(), true);
    set_nonblocking(read_end.get(), true);
    read_fd_ = std::move(read_end);
    write_fd_ = std::move(write_end);
    g_signal_write_fd = write_fd_.get();

    struct sigaction action {};
    action.sa_handler = jobrun_signal_handler;
    sigemptyset(&action.sa_mask);
    // 【知识点：故意不设 SA_RESTART】我们要的就是「打断阻塞中的系统调用」。
    // 设上 SA_RESTART 之后，被信号打断的 poll/read 会被内核自动重启，
    // 我们就失去了「立刻响应 Ctrl-C」的机会，只能等它自己超时。
    action.sa_flags = 0;

    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) {
        if (::sigaction(sig, &action, nullptr) < 0) {
            fail("sigaction");
        }
    }
}

SignalWatcher::~SignalWatcher() {
    // 先恢复默认处置，再拆管道：反过来的话，处理器可能访问到已经关掉的 fd。
    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) {
        ::signal(sig, SIG_DFL);
    }
    g_signal_write_fd = -1;
}

int SignalWatcher::poll_once(int timeout_ms) {
    if (!read_fd_.valid()) {
        return 0;
    }
    pollfd pfd{};
    pfd.fd = read_fd_.get();
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc <= 0) {
        return 0;  // 超时、或者被信号打断 —— 两种都只是「这次没读到东西」
    }
    int last = 0;
    unsigned char buffer[64];
    for (;;) {
        const ssize_t n = ::read(read_fd_.get(), buffer, sizeof buffer);
        if (n > 0) {
            last = buffer[n - 1];  // 一次收到多个信号时，以最后一个为准
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;  // EAGAIN：读干净了
    }
    return last;
}

}  // namespace jobrun
