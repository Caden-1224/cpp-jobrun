// ============================================================================
//  signals.cpp —— 自管道技巧的实现
//
// ============================================================================
//  【这个文件教什么】
//  ============================================================================
//  * extern "C"：让 C++ 函数拥有 C 的链接名
//  * volatile / sig_atomic_t：信号处理器能安全访问的极少数类型
//  * errno 的保存与恢复
//  * async-signal-safe：处理器里到底能调用什么
//  * sigaction 的字段：sa_handler / sa_mask / sa_flags
//  * 自管道技巧：把异步信号变成同步的「可 poll 事件」
//
// ============================================================================
//  【给只会 C 的你：C 里也一样，只是没人讲清楚】
//  ============================================================================
//
//  C 里写信号处理器，教科书常给这个例子：
//      void handler(int sig) { printf("got %d\n", sig); }
//  这是错的。printf 不是 async-signal-safe：
//      * 它可能正在被主线程使用，内部锁已经锁住；
//      * 它可能触发 malloc / locale 初始化；
//      * 处理器打断的可能是 malloc 的临界区。
//  结果就是随机死锁或堆损坏 —— 而且极难复现。
//
//  POSIX 列出的 async-signal-safe 函数很少：read、write、open、close、
//  _exit、kill、sigaction…… 其中 write 是最常用的「通知」手段。
//  所以标准做法是：处理器只 write 一个字节，主循环去 poll/read 这根管道。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. extern "C"
//         extern "C" void jobrun_signal_handler(int sig) { ... }
//     C++ 编译器会对函数名做「名字修饰」（name mangling，例如
//     _Z21jobrun_signal_handleri）。extern "C" 关掉修饰，让符号名就是
//     jobrun_signal_handler，这样 C 代码/内核回调能按名字找到它。
//     【C 对照】C 本来就没有名字修饰；extern "C" 是让 C++ 假装成 C。
//
//  2. volatile sig_atomic_t
//     标准规定：信号处理器里能安全访问的，只有 volatile sig_atomic_t
//     类型的对象（以及无锁原子类型）。
//     【本文件的取舍】g_signal_write_fd 只在安装处理器之前写入、
//     在卸载处理器之后清空，处理器运行期间它保持不变，所以没有并发写。
//     严格来说仍应声明为 volatile sig_atomic_t；这里用 int 是因为
//     fd 的取值范围和读写时机都可控，属于「实践中安全，标准上不严谨」。
//     如果你要改这段代码，建议改成：
//         volatile sig_atomic_t g_signal_write_fd = -1;
//
//  3. static_cast<unsigned char>(sig)
//     write() 的第二个参数是 const void*，需要取地址。这里把 int 信号号
//     转成 unsigned char 再写 1 字节：信号号 1..64 都能放进一个字节。
//     【C 对照】C 里直接 (unsigned char)sig 也可以，但 C++ 建议用
//     static_cast，编译器会检查转换是否合法。
//
//  4. (void)ignored;
//     显式「使用」一个变量来消除「未使用变量」警告。
//     C 里也常见 (void)x; 这是跨语言通用的写法。
//
//  5. struct sigaction action {};
//     花括号初始化把整个结构体清零（value-initialization）。
//     【坑】不初始化的话，sa_flags/sa_mask 里是栈上的垃圾值，行为随机。
//
//  6. range-for over braced-init-list
//         for (const int sig : {SIGINT, SIGTERM, SIGHUP})
//     {a, b, c} 会推导成 std::initializer_list<int>，可以像数组一样遍历。
//     【C 对照】C 里要写数组 + sizeof/sizeof。
//
//  7. std::_Exit vs std::exit
//     std::_Exit(130) 直接进内核，不跑 atexit、不 flush stdio；
//     std::exit(130) 会跑清理。信号处理路径上要的是前者。
//     （POSIX 的 _exit 和 C++ 的 std::_Exit 行为一致。）
//
// ============================================================================
//  【本文件涉及的 POSIX API：参数、返回、errno、扩展】
// ============================================================================
//
//  int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
//      signum : 要操作的信号
//      act    : 新处置；NULL = 只查询
//      oldact : 旧处置；NULL = 不关心
//      返回   : 0 成功；-1 + errno（EINVAL 无效信号、EFAULT 坏指针）
//      struct sigaction:
//          sa_handler   void (*)(int)
//          sa_sigaction void (*)(int, siginfo_t*, void*)  需要 SA_SIGINFO
//          sa_mask      sigset_t，处理器执行期间额外屏蔽的信号
//          sa_flags     SA_RESTART / SA_NODEFER / SA_RESETHAND / SA_SIGINFO
//      【坑】sa_mask 必须用 sigemptyset() 初始化；memset 0 不保证是空集。
//
//  int sigemptyset(sigset_t *set);
//      把信号集清空。返回 0 / -1。相关：sigfillset、sigaddset、sigdelset、
//      sigismember。
//
//  sighandler_t signal(int signum, sighandler_t handler);
//      老接口。SIG_DFL 恢复默认，SIG_IGN 忽略。行为在不同 Unix 上有差异，
//      新代码应优先用 sigaction。本文件只用它来「恢复默认」，够了。
//
//  ssize_t write(int fd, const void *buf, size_t count);
//      【规则】返回值可能小于 count（short write），必须循环；
//             返回 -1 + errno（EAGAIN 表示非阻塞写满、EINTR 表示被打断）。
//      【规则】write 是 async-signal-safe 的。
//      【扩展】writev（一次写多个缓冲区）、pwrite（带偏移）、sendfile。
//
//  ssize_t read(int fd, void *buf, size_t count);
//      返回 0 表示 EOF；>0 是读到的字节数；-1 + errno。
//      【规则】read 也是 async-signal-safe 的。
//
//  int poll(struct pollfd *fds, nfds_t nfds, int timeout);   （见 process.hpp）
//
//  【扩展】signalfd(2)：先 sigprocmask 屏蔽信号，再把它变成一个可 poll 的 fd；
//          sigwait(3)：同步等待信号，适合专门线程；
//          ppoll(2)：poll + 原子替换信号掩码，避免竞态。
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
///
/// 【严格规则】C/C++ 标准只保证 volatile sig_atomic_t（以及无锁原子类型）
/// 能在信号处理器里安全访问。见文件头的说明。
int g_signal_write_fd = -1;

// 【语法】extern "C"：让这个函数的符号名保持 C 风格，不做名字修饰。
//         sigaction 接收的是函数指针；用 C 链接能保证和 POSIX 的约定一致。
extern "C" void jobrun_signal_handler(int sig) {
    // 【知识点：保护 errno】处理器打断了正常代码，而正常代码可能正要读 errno。
    // 我们的 write() 会改写 errno，所以进来先存、出去再恢复。
    //
    // 【语法】errno 在 POSIX 上是「线程局部」的（每个线程一份），
    //         但信号处理器运行在被打断的那个线程上，所以必须保存/恢复。
    const int saved_errno = errno;
    // 【语法】static_cast<unsigned char>：把 int 窄化成 1 字节。
    //         信号号通常 1..64，一个字节足够。
    const unsigned char byte = static_cast<unsigned char>(sig);
    if (g_signal_write_fd >= 0) {
        // 写端是非阻塞的：万一管道满了，write 立刻返回 EAGAIN，
        // 处理器绝不会在这里睡过去。（在这里阻塞等于把整个进程冻住。）
        //
        // 【语法】(void)ignored 显式丢弃返回值，避免 -Wunused-result 警告。
        //         write 的返回值这里确实不关心：能写就写，写不进去也不重试。
        const ssize_t ignored = ::write(g_signal_write_fd, &byte, 1);
        (void)ignored;
    }
    errno = saved_errno;
}

}  // namespace

std::string signal_name(int sig) {
    // 【语法】switch 覆盖常用信号；default 兜住其余。
    //         std::to_string 把 int 转成 string。
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
    //
    // 【语法】auto [read_end, write_end] = ...; 是 C++17 的结构化绑定：
    //         把 pair 的两个成员分别绑定到两个局部变量上。
    auto [read_end, write_end] = Fd::make_pipe();
    set_nonblocking(write_end.get(), true);
    set_nonblocking(read_end.get(), true);
    // 【语法】std::move 把 Fd 搬进成员（Fd 的移动构造见 fd.cpp）。
    //         搬完之后 read_end/write_end 变成空句柄，析构时什么也不做。
    read_fd_ = std::move(read_end);
    write_fd_ = std::move(write_end);
    g_signal_write_fd = write_fd_.get();

    // 【语法】struct sigaction action {}; 花括号初始化，整个结构体清零。
    //         不写 {} 的话，sa_flags/sa_mask 是栈上的垃圾值。
    struct sigaction action {};
    action.sa_handler = jobrun_signal_handler;
    // 【API】sigemptyset 把信号集清空。必须调用，不能靠 memset。
    sigemptyset(&action.sa_mask);
    // 【知识点：故意不设 SA_RESTART】我们要的就是「打断阻塞中的系统调用」。
    // 设上 SA_RESTART 之后，被信号打断的 poll/read 会被内核自动重启，
    // 我们就失去了「立刻响应 Ctrl-C」的机会，只能等它自己超时。
    //
    // 【扩展】SA_NODEFER：处理器执行时不自动屏蔽当前信号（会递归）；
    //         SA_RESETHAND：进入处理器后把处置重置为 SIG_DFL；
    //         SA_SIGINFO：使用 sa_sigaction 而不是 sa_handler。
    action.sa_flags = 0;

    // 【语法】range-for 遍历 braced-init-list：
    //         {SIGINT, SIGTERM, SIGHUP} 推导为 initializer_list<int>。
    //         这里按值拷贝 int，没有开销。
    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) {
        if (::sigaction(sig, &action, nullptr) < 0) {
            // 【语法】fail() 是 [[noreturn]]，所以这里不需要 return。
            fail("sigaction");
        }
    }
}

SignalWatcher::~SignalWatcher() {
    // 先恢复默认处置，再拆管道：反过来的话，处理器可能访问到已经关掉的 fd。
    // 【语法】这里用 signal() 只是为了「恢复 SIG_DFL」，不是为了安装新处理器。
    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) {
        ::signal(sig, SIG_DFL);
    }
    // 【语法】置 -1 之后，即使还有处理器在跑，write 也会被 if 挡住。
    g_signal_write_fd = -1;
    // 【语法】析构函数体结束后，read_fd_/write_fd_ 的析构函数自动 close。
    //         注意顺序：成员按「声明逆序」析构，所以 write_fd_ 先于 read_fd_ 关闭。
}

int SignalWatcher::poll_once(int timeout_ms) {
    if (!read_fd_.valid()) {
        return 0;
    }
    // 【语法】pollfd pfd{}; 聚合体零初始化：fd=0, events=0, revents=0。
    //         然后逐个字段赋值。
    pollfd pfd{};
    pfd.fd = read_fd_.get();
    pfd.events = POLLIN;
    // 【API】poll 返回 >0 表示有 fd 就绪，0 表示超时，-1 表示出错。
    //        这里 rc <= 0 一律当作「这次没读到东西」：超时是正常的，
    //        EINTR 也只需要下一轮再试。
    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc <= 0) {
        return 0;  // 超时、或者被信号打断 —— 两种都只是「这次没读到东西」
    }
    int last = 0;
    // 【语法】unsigned char buffer[64]; 栈上数组。读端是非阻塞的，
    //         所以这个循环不会卡住。
    unsigned char buffer[64];
    for (;;) {
        const ssize_t n = ::read(read_fd_.get(), buffer, sizeof buffer);
        if (n > 0) {
            // 【语法】buffer[n - 1]：取本次读到的最后一个字节。
            //         如果一次积压了多个信号，以最后一个为准。
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
