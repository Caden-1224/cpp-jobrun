// ============================================================================
//  process.cpp —— 真实子进程：fork / exec / pipe / poll / waitpid / kill
//
// ============================================================================
//  【这个文件教什么】
//  ============================================================================
//  这是整份练习里最「操作系统」、也最接近 C 的文件。它把七件事串起来：
//      1. pipe2 建管道
//      2. fork 复制出一个子进程
//      3. 子进程 setpgid 自成一个进程组
//      4. 子进程 dup2 把管道接到 stdout/stderr
//      5. 子进程 execvp 换成目标程序
//      6. 父进程 poll + 非阻塞 read 持续抽干两根管道
//      7. 超时/取消时 kill(-pgid) 整组杀，再 waitpid 回收
//
// ============================================================================
//  【给只会 C 的你：这个文件里的 C++ 语法其实很少】
//  ============================================================================
//  本文件的系统调用写法和 C 几乎一样（::fork、::execvp、::poll……）。
//  真正属于 C++ 的只有这些：
//      * 匿名 namespace / constexpr 常量
//      * std::string 的 find / substr / erase
//      * std::vector<char*> 和 std::optional
//      * 结构化绑定 auto [a, b] = ...
//      * 一次 const_cast（为了适配 execvp 的老接口）
//      * RAII（Fd / ChildProcess）把 close/waitpid 变成自动行为
//
//  换句话说：你已有的 C 知识在这里 100% 有效；C++ 只是帮你把「资源管理」
//  和「字符串/容器」这几件杂事接了过去。
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
//  ============================================================================
//
//  1. constexpr 常量
//         constexpr int kPollSliceMs = 100;
//     编译期常量。C 里用 #define 或 enum；constexpr 有类型、有作用域、
//     能参与重载，比宏安全。
//
//  2. 匿名 namespace
//         namespace { ... }
//     里面的函数/类型只在本翻译单元可见，等价于 C 的 static。
//
//  3. 栈数组
//         char buf[kReadChunk];
//     kReadChunk 是 constexpr，所以这是标准 C++ 的定长数组。
//     【坑】C99 的变长数组（int a[n]，n 是运行时变量）不是标准 C++，
//          GCC 会以扩展形式接受，但用 -Wpedantic 会警告。需要动态大小
//          就用 std::vector<char>。
//
//  4. std::string 的查找与切片
//         ch.pending.find('\n', start)  // 找不到返回 std::string::npos
//         ch.pending.substr(start, len) // 返回新字符串
//         ch.pending.erase(0, start)    // 原地删除前缀
//     npos 是 size_t 的最大值，不能和 -1 直接比较（类型不同）。
//
//  5. 结构化绑定
//         auto [out_read, out_write] = Fd::make_pipe();
//     把 pair/tuple/聚合体的成员拆成独立变量，C++17 起可用。
//
//  6. const_cast
//         const_cast<char*>(arg.c_str())
//     去掉 const。这里是为了适配 execvp 的 char* const argv[]（它虽然
//     不修改字符串，但签名不是 const）。这是少数「合法但难看」的用法。
//     【规则】const_cast 只能去掉/加上 const，不能改变底层类型。
//
//  7. 子进程里的 _exit
//         ::_exit(127);
//     不 flush stdio、不跑 atexit。原因见下面 exec 失败那段注释。
//
//  8. std::optional
//         std::optional<ExitStatus> try_reap();
//     用 nullopt 表示「还没有」，避免用 pid == -1 之类的哨兵值。
//
// ============================================================================
//  【本文件涉及的 POSIX API：参数、返回、errno、扩展】
//  ============================================================================
//
//  pid_t fork(void);
//      返回：父进程 > 0（子进程 pid）；子进程 0；失败 -1 + errno。
//      【关键】父子各返回一次。子进程是完整副本（copy-on-write），
//      fd 表、信号处置、进程组、当前目录全部继承。
//      扩展：vfork / posix_spawn / clone。
//
//  int pipe2(int pipefd[2], int flags);
//      见 fd.hpp。
//
//  int dup2(int oldfd, int newfd);
//      返回 newfd / -1。详见 process.hpp 的 API 注释。
//
//  int execvp(const char *file, char *const argv[]);
//      成功不返回；失败 -1 + errno。argv 必须 NULL 结尾。
//
//  int setpgid(pid_t pid, pid_t pgid);
//      返回 0 / -1 + errno（EACCES/ESRCH/EPERM）。
//
//  int kill(pid_t pid, int sig);
//      pid < 0 表示「进程组 |pid|」。返回 0 / -1 + errno。
//
//  pid_t waitpid(pid_t pid, int *wstatus, int options);
//      返回 >0 / 0（WNOHANG）/ -1 + errno。
//
//  int poll(struct pollfd *fds, nfds_t nfds, int timeout);
//      返回 >0 / 0 / -1 + errno。
//
//  ssize_t read(int fd, void *buf, size_t count);
//      返回 >0 / 0（EOF）/ -1 + errno。
//
//  ssize_t write(int fd, const void *buf, size_t count);
//      返回 >0 / -1 + errno。
// ============================================================================
#include "jobrun/process.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace jobrun {
namespace {

// 【语法】using 别名，缩短类型名。
using Clock = std::chrono::steady_clock;

/// 每次 poll 最多等这么久。分片轮询有两个好处：
/// 一是有机会检查「用户按了 Ctrl-C 吗」，二是有机会检查「超时了吗」。
/// 如果在这里写 -1（无限等），取消和超时就都失效了。
///
/// 【语法】constexpr 是编译期常量，编译器可以把它直接内联进代码。
/// 【C 对照】C 里写 #define POLL_SLICE_MS 100 或 enum { ... }。
constexpr int kPollSliceMs = 100;

/// 读一块的缓冲区大小。
constexpr std::size_t kReadChunk = 16 * 1024;

/// 单行长度上限。防止某个作业吐出一行几百 MB 的文本把我们自己撑爆。
constexpr std::size_t kMaxLineBytes = 64 * 1024;

/// 进入「输出样本」的每一行最多留这么多个字节。
/// 样本是用来看的，不是用来存的 —— 一行 200 KB 的 'x' 打印出来只会刷屏。
constexpr std::size_t kMaxPreviewChars = 160;

/// 发出 SIGKILL 之后，最多再等这么久把管道排空。
/// 正常情况下瞬间就 EOF；留这个上限是为了对付「有孙子逃出了进程组」。
constexpr int kDrainGraceMs = 2000;

/// 计算两个时间点之间的微秒数。
///
/// 【语法】duration_cast<microseconds> 把任意精度的时长转成微秒；
///         .count() 取出底层整数。
/// 【扩展】chrono 还有 duration_cast<milliseconds>、<nanoseconds>，
///         以及 C++20 的 std::chrono::floor/ceil/round。
long long elapsed_us(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
}

/// 把 waitpid 的 int status 解码成「怎么结束的」。
///
/// 【语法】WIFEXITED(status) 等宏来自 <sys/wait.h>，它们对 status 做位测试。
///         这些宏要求传入一个 int 左值（宏内部会取地址），所以不能直接把
///         waitpid(...) 的返回值塞进去；先存到变量里再判断。
/// 【C 对照】C 里完全一样；这里只是把结果装进一个结构体，方便跨函数传递。
ExitStatus decode_status(int status) {
    ExitStatus s;
    s.reaped = true;
    if (WIFEXITED(status)) {
        s.exited = true;
        s.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        s.signaled = true;
        s.term_signal = WTERMSIG(status);
    }
    // 【扩展】WIFSTOPPED/WSTOPSIG（被暂停）、WIFCONTINUED（被恢复），
    //         需要 waitpid 的 WUNTRACED/WCONTINUED 选项才会报告。
    return s;
}

/// 兜底回收：析构、移动赋值都用它，保证绝不留僵尸。
///
/// 【语法】noexcept：这个函数会被析构函数调用，而析构不能抛异常。
/// 【语法】void force_reap(pid_t pid) noexcept 定义在匿名 namespace 里，
///         外部链接不可见。
void force_reap(pid_t pid) noexcept {
    if (pid <= 0) {
        return;
    }
    // 先 SIGTERM（让有清理逻辑的程序有机会跑），再立刻 SIGKILL（保证它一定死）。
    // 两者之间没有等待，所以清理逻辑多半来不及 —— 这里只是尽力而为，
    // 真正重要的是「不能留僵尸」。
    //
    // 【API】kill(-pid, sig)：发给「进程组 pid」的所有成员；
    //         kill(pid, sig)：只发给直接子进程。
    //         两边都发，是为了覆盖「进程组还没建立」的窄窗口。
    ::kill(-pid, SIGTERM);
    ::kill(pid, SIGTERM);
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    int status = 0;
    // 【规则】waitpid 可能被信号打断（EINTR），必须重试。
    //         这里不关心 status，只要把孩子收掉就行。
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

/// 一根管道上的读取进度。
///
/// 【语法】这是一个聚合体，成员都有默认值；Channel out; 就能用。
struct Channel {
    Fd* fd = nullptr;
    std::size_t bytes = 0;
    std::size_t lines = 0;
    bool eof = false;
    bool truncated = false;
    /// 正在丢弃一个超长行的剩余部分？
    ///
    /// 【知识点：为什么要单独设一个状态】如果只是「pending 超过 64 KB 就
    /// 当一行结算掉」，那么一行 200 KB 的文本会被切成 3 段、算成 3 行 ——
    /// 明明只有一行。所以超长行的处理是「先记一行，然后一直丢到下一个换行符」，
    /// 而不是「每满 64 KB 就记一行」。
    bool discarding = false;
    std::string pending;               ///< 还没凑齐一整行的尾巴
    std::vector<std::string> preview;  ///< 只留前 N 行做样本
    std::size_t preview_limit = 4;
};

/// 把一行裁成适合放进样本的长度。
///
/// 【知识点：别把 UTF-8 字符切两半】UTF-8 里 0b10xxxxxx 是「续字节」，
/// 只有首字节才是一个字符的开头。所以按字节截断之后要往回退，
/// 退到某个首字节上为止 —— 否则会印出乱码。
///
/// 【语法】static_cast<unsigned char>(line[cut])：先转无符号再和 0xC0 做与，
///         避免 char 是 signed 时高位被符号扩展。
std::string clip_for_preview(const std::string& line) {
    if (line.size() <= kMaxPreviewChars) {
        return line;
    }
    std::size_t cut = kMaxPreviewChars;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    // 【语法】substr(0, cut) 返回前缀；+ 拼接字符串字面量。
    return line.substr(0, cut) + " …（本行过长，样本已截断）";
}

/// 把 pending 里所有完整的行结算掉。
///
/// 【算法】
///   * 从 start 开始找 '\n'；
///   * 每找到一个，就是一行；如果不在丢弃模式，就计数、存样本；
///   * 最后把已结算的前缀 erase 掉，剩下的半行继续等下一块数据。
///
/// 【坑】超长行进入 discarding 模式后，pending 会被清空，只保留「找换行符」
///       这一个目的；下一个换行符出现时退出丢弃模式。
void harvest_lines(Channel& ch) {
    std::size_t start = 0;
    for (;;) {
        // 【语法】std::string::npos 是「没找到」的哨兵值。
        //         注意它是 size_t，不要和 -1 比较。
        const std::size_t nl = ch.pending.find('\n', start);
        if (nl == std::string::npos) {
            break;
        }
        if (!ch.discarding) {
            if (ch.preview.size() < ch.preview_limit) {
                // 【语法】substr(start, nl - start)：从 start 开始、长度 nl-start，
                //         正好是这一行（不含换行符）。
                ch.preview.push_back(clip_for_preview(ch.pending.substr(start, nl - start)));
            }
            ++ch.lines;
        }
        ch.discarding = false;  // 无论之前是不是在丢弃，这一行的尾巴都找到了
        start = nl + 1;
    }
    if (start > 0) {
        // 【语法】erase(pos, count) 原地删除；这里删掉已经结算的前缀。
        //         O(剩余长度) 的搬移，但 pending 通常只有一行大小。
        ch.pending.erase(0, start);
    }

    if (ch.pending.size() > kMaxLineBytes) {
        if (!ch.discarding) {
            // 一整行太长：按「一行」记下它的开头，然后转成丢弃模式，
            // 一直丢到下一个换行符为止 —— 否则这一行会被数成好几行。
            if (ch.preview.size() < ch.preview_limit) {
                ch.preview.push_back(clip_for_preview(ch.pending));
            }
            ++ch.lines;
            ch.truncated = true;
            ch.discarding = true;
        }
        // 丢弃模式下的 pending 只是为了找换行符，不必真的留着它
        ch.pending.clear();
    }
}

/// 把一根管道里「现在能读到的」全部读走。
///
/// 【知识点：为什么可以循环读到 EAGAIN】读端在父进程里已经被设为非阻塞。
/// 如果是阻塞 fd，这个循环会卡在最后一次 read 上等数据 —— 那就轮到我们
/// 自己阻塞了，poll 的意义也就没了。
///
/// 【参数】ch.fd 必须是一个有效的、非阻塞的读端。
void drain_fd(Channel& ch) {
    // 【语法】栈上 16 KiB 缓冲区。kReadChunk 是 constexpr，所以是定长数组。
    char buf[kReadChunk];
    for (;;) {
        // 【API】read 返回 >0 字节数、0 EOF、-1 + errno。
        const ssize_t n = ::read(ch.fd->get(), buf, sizeof buf);
        if (n > 0) {
            // 【语法】static_cast<std::size_t>(n)：n 是 ssize_t（有符号），
            //         这里已经确定 > 0，转换安全。
            ch.bytes += static_cast<std::size_t>(n);
            // 【语法】append(ptr, len) 追加一段可能含 '\0' 的字节；
            //         不要用 += 字符串，因为输出里可能真的有 NUL。
            ch.pending.append(buf, static_cast<std::size_t>(n));
            harvest_lines(ch);
            continue;
        }
        if (n == 0) {
            // EOF —— 所有持有写端的进程都关掉了它，包括子进程和它的子孙。
            ch.eof = true;
            if (!ch.pending.empty()) {
                // 【知识点：最后一行可能没有换行符】`printf 'abc'` 只吐出三个字节，
                // 一个 '\n' 都没有。如果不在这儿补一刀，这一行会被整条漏掉 ——
                // 而它往往正是程序最想说的那句话。
                if (!ch.discarding) {
                    if (ch.preview.size() < ch.preview_limit) {
                        ch.preview.push_back(clip_for_preview(ch.pending));
                    }
                    ++ch.lines;
                }
                ch.pending.clear();
            }
            return;
        }
        if (errno == EINTR) {
            continue;  // 被信号打断是常态，重来即可
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  // 暂时读空了，回去 poll
        }
        // 【规则】其它错误（EIO 等）说明这根管子废了。标记 EOF，不再等它。
        ch.eof = true;
        return;
    }
}

}  // namespace

const char* to_string(RunState state) noexcept {
    // 【语法】switch 覆盖全部枚举值；每个分支直接 return。
    //         如果以后加了新枚举值，-Wswitch 会提醒你这里漏了。
    switch (state) {
        case RunState::Succeeded: return "成功";
        case RunState::Failed: return "失败";
        case RunState::Signaled: return "被信号终止";
        case RunState::TimedOut: return "超时";
        case RunState::Cancelled: return "已取消";
        case RunState::SpawnError: return "启动失败";
    }
    return "未知";
}

std::string ExitStatus::describe() const {
    // 【语法】std::to_string 把数字转成 string，支持 int/long/long long/float 等。
    if (signaled) {
        return "被信号 " + std::to_string(term_signal) + " 终止";
    }
    if (exited) {
        return "退出码 " + std::to_string(code);
    }
    return "未回收";
}

// ============================== ChildProcess ==============================

ChildProcess::ChildProcess(pid_t pid) noexcept : pid_(pid) {}

ChildProcess::~ChildProcess() {
    // 【知识点：析构即兜底】正常路径上 reap()/try_reap() 已经把 reaped_ 置位，
    // 这里什么也不做。只有「谁忘了回收」时才由析构函数接管 ——
    // 这正是 RAII 的价值：把「不能忘的事」交给编译器安排。
    if (pid_ > 0 && !reaped_) {
        force_reap(pid_);
    }
}

// 【语法】移动构造：接管 other 的 pid 和状态，然后把 other 置为「空句柄」。
//         这里的成员初始化列表按声明顺序写：pid_ 再 reaped_。
ChildProcess::ChildProcess(ChildProcess&& other) noexcept : pid_(other.pid_), reaped_(other.reaped_) {
    other.pid_ = -1;
    other.reaped_ = true;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    // 【语法】自赋值检查，和 Fd::operator= 同理。
    if (this != &other) {
        // 【规则】先处理自己原来的孩子，再接管新的；否则旧 pid 会泄漏。
        if (pid_ > 0 && !reaped_) {
            force_reap(pid_);
        }
        pid_ = other.pid_;
        reaped_ = other.reaped_;
        other.pid_ = -1;
        other.reaped_ = true;
    }
    return *this;
}

void ChildProcess::signal_group(int sig) const {
    if (pid_ <= 0) {
        return;
    }
    // 【知识点：负 pid = 进程组】kill(-pgid, sig) 把信号发给整个组。
    if (::kill(-pid_, sig) == 0) {
        return;
    }
    // ESRCH 表示这个组还不存在：从 fork 返回到子进程执行 setpgid 之间
    // 有一个极窄的窗口。这时退化成只杀它自己，聊胜于无。
    //
    // 【扩展】Linux 5.3+ 的 pidfd_send_signal 可以避免 pid 复用的竞态：
    //         你以为在杀 A，其实 A 已经退出、pid 被 B 复用。
    if (errno == ESRCH) {
        ::kill(pid_, sig);
    }
}

std::optional<ExitStatus> ChildProcess::try_reap() noexcept {
    if (pid_ <= 0 || reaped_) {
        return std::nullopt;
    }
    int status = 0;
    // 【API】WNOHANG：没有已结束的子进程时立刻返回 0，不阻塞。
    const pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0) {
        return std::nullopt;  // 还在跑
    }
    reaped_ = true;
    if (r < 0) {
        return ExitStatus{};  // ECHILD 之类：已经被回收过了
    }
    return decode_status(status);
}

ExitStatus ChildProcess::reap() noexcept {
    if (pid_ <= 0 || reaped_) {
        return ExitStatus{};
    }
    int status = 0;
    pid_t r = -1;
    // 【规则】waitpid 被信号打断时返回 -1 + EINTR，必须重试。
    do {
        r = ::waitpid(pid_, &status, 0);
    } while (r < 0 && errno == EINTR);
    reaped_ = true;
    if (r < 0) {
        return ExitStatus{};
    }
    return decode_status(status);
}

// =========================== run_command_captured ===========================

CaptureResult run_command_captured(const std::vector<std::string>& argv, const CaptureOptions& options) {
    if (argv.empty()) {
        // 【语法】throw 一个标准异常；调用方用 catch (const std::exception&) 接住。
        //         invalid_argument 表示「参数不合法」。
        throw std::invalid_argument("run_command_captured: argv 为空");
    }

    // ---- fork 之前，把所有会分配内存的事情做完 ----
    // 【知识点：fork 之后不能碰 malloc】fork 只复制调用它的那个线程，
    // 但其它线程手里的锁全都被复制成「已加锁」状态，且永远不会有人来解锁。
    // 所以在子进程里 new / std::string / iostream / printf 都可能死锁。
    // 结论：argv 必须在 fork 之前就转成 char* 数组。
    //
    // 【语法】std::vector<char*> raw; raw.reserve(...) 预留容量，
    //         避免 push_back 过程中重新分配（虽然这里还没 fork，但好习惯）。
    // 【语法】const_cast<char*>(arg.c_str())：execvp 的签名是 char* const[]，
    //         它不会修改字符串，但类型上不是 const。这是为了兼容老接口。
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
        raw.push_back(const_cast<char*>(arg.c_str()));
    }
    // 【API】argv 必须以 nullptr 结尾，execvp 靠它判断参数结束。
    raw.push_back(nullptr);

    // 【语法】结构化绑定：把 pair<Fd, Fd> 拆成两个 Fd。
    auto [out_read, out_write] = Fd::make_pipe();
    auto [err_read, err_write] = Fd::make_pipe();
    // 第三根管道专门用来回答一个问题：exec 到底成功了没有？
    //
    // 【知识点：CLOEXEC 的巧妙用法】它的写端带 O_CLOEXEC。
    //   * exec 成功 -> 内核在切换映像时自动关掉这个写端 -> 父进程读到 EOF(0 字节)
    //   * exec 失败 -> 子进程抢在自己 _exit 之前把 errno 写进去 -> 父进程读到 >0 字节
    // 于是「读到 0 字节」这件事本身，就成了 exec 成功的证据。
    // 这比「用退出码 127 代表 command not found」可靠得多。
    //
    // 【坑】这根管道的读端必须保持阻塞：父进程要在这里等一个确定的结论。
    auto [exec_read, exec_write] = Fd::make_pipe();

    const auto started = Clock::now();

    // 【API】fork 返回两次：父进程拿到子进程 pid，子进程拿到 0，失败返回 -1。
    const pid_t pid = ::fork();
    if (pid < 0) {
        fail("fork");
    }

    if (pid == 0) {
        // ============================== 子进程 ==============================
        // 从这里到 execvp 之间，只能调用 async-signal-safe 的函数。
        // 具体原因见上面「fork 之后不能碰 malloc」的说明。

        // 1. 自成一个进程组，组长是自己。
        // 【API】setpgid(0, 0)：0 表示当前进程，第二个 0 表示「用 pid 当组号」。
        // 【规则】这样 kill(-pid) 才能一次带走 sh 和它 fork 的 sleep。
        ::setpgid(0, 0);

        // 2. 把被父进程改过的信号处置恢复成默认。
        // 【知识点：信号处置会被 fork 继承】如果父进程把 SIGINT 设成了
        // 「忽略」，子进程会原样继承这个「忽略」，于是 Ctrl-C 对它毫无作用。
        // 我们必须显式恢复，子进程才回到正常的默认行为。
        //
        // 【语法】::signal 是 C 接口；这里只需要「设为 SIG_DFL」。
        //         严格来说 sigaction 更可移植，但子进程里要尽量少做事。
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGPIPE, SIG_DFL);

        // 3. 重定向标准输出与标准错误。
        // 【知识点：dup2 会清掉新 fd 上的 CLOEXEC】正因为如此，
        // 我们这两根管道才能活过 exec；而其它所有 fd 都带 CLOEXEC，
        // exec 时被内核顺手关掉 —— 包括别的作业的管道。
        //
        // 【API】dup2(oldfd, newfd)：让 newfd 变成 oldfd 的副本；
        //         如果 newfd 已打开，先原子地关闭它。
        //         STDOUT_FILENO=1，STDERR_FILENO=2，定义在 <unistd.h>。
        ::dup2(out_write.get(), STDOUT_FILENO);
        ::dup2(err_write.get(), STDERR_FILENO);

        // 4. 换程序。
        // 【API】execvp(file, argv)：
        //         - file 不含 '/' 时按 PATH 搜索；
        //         - argv[0] 是程序名，argv 以 NULL 结尾；
        //         - 成功不返回，失败返回 -1 并设置 errno。
        ::execvp(raw[0], raw.data());

        // 只有 exec 失败才会执行到这里。
        //
        // 【语法】先把 errno 存下来：后面调用 write 可能覆盖 errno。
        const int exec_errno = errno;
        // 【API】把 errno 写进探测管道。父进程读到 >0 字节就知道 exec 失败。
        //         write 是 async-signal-safe 的，子进程里可以安全调用。
        ssize_t ignored = ::write(exec_write.get(), &exec_errno, sizeof(exec_errno));
        (void)ignored;
        // 【知识点：_exit 而不是 exit】exit() 会跑 atexit 处理器、并 flush
        // 继承自父进程的 stdio 缓冲区 —— 那会把父进程攒着还没打印的内容
        // 在这里重复打印一遍。_exit 直接进内核，什么都不跑。
        //
        // 【语法】::_exit 是 POSIX 接口；C++ 标准库对应的是 std::_Exit。
        ::_exit(127);
    }

    // ============================== 父进程 ==============================

    // 与子进程里的 setpgid 构成「双向竞赛」：两边都设置一遍，
    // 无论谁先跑到，进程组都已经就绪，不存在竞态窗口。
    //
    // 【知识点：为什么要两边都设】如果只让子进程设，父进程可能在子进程
    // 执行 setpgid 之前就调用 kill(-pid)，此时进程组还不存在（ESRCH）。
    // 两边都设，正常情况下两次都成功（setpgid 是幂等的）；如果子进程已经
    // exec，后到的一次可能返回 EACCES，忽略即可。
    ::setpgid(pid, pid);

    // 【知识点：必须关掉写端】管道的内核缓冲区由「还有没有写端」决定生死。
    // 父进程如果攥着写端不放，即使子进程早就退出，read() 也永远等不到 EOF。
    //
    // 【语法】reset() 关闭并置空；Fd 的析构也会做同样的事，但这里必须
    //         在 fork 之后立刻做，不能等作用域结束。
    out_write.reset();
    err_write.reset();
    exec_write.reset();  // 不关这个，上面那个「EOF 即成功」的技巧就不成立了

    ChildProcess child(pid);

    // 等 exec 的结论。这一步不会挂住：exec 要么立刻成功（EOF），
    // 要么立刻失败（写回 errno）。
    //
    // 【语法】花括号限定 value/n 的作用域，避免和后面的变量重名。
    CaptureResult result;
    int spawn_errno = 0;
    {
        int value = 0;
        ssize_t n = 0;
        // 【规则】read 可能被信号打断，EINTR 要重试。
        do {
            n = ::read(exec_read.get(), &value, sizeof(value));
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            spawn_errno = value;
        }
        // n == 0 表示 EOF：exec 成功（内核关掉了带 CLOEXEC 的写端）。
    }

    if (spawn_errno != 0) {
        // exec 失败：子进程已经 _exit(127)，回收掉它，然后如实报告。
        result.state = RunState::SpawnError;
        result.spawn_errno = spawn_errno;
        child.reap();
        result.duration_us = elapsed_us(started, Clock::now());
        return result;
    }

    // 读端设为非阻塞，配合 poll 才能「有多少读多少」而不卡住自己。
    set_nonblocking(out_read.get(), true);
    set_nonblocking(err_read.get(), true);

    // 【语法】Channel 的 fd 成员是 Fd*，指向局部的 out_read/err_read。
    //         这两个 Fd 活到函数结束，所以指针始终有效。
    Channel out;
    out.fd = &out_read;
    out.preview_limit = options.max_preview_lines;
    Channel err;
    err.fd = &err_read;
    err.preview_limit = options.max_preview_lines;

    bool timed_out = false;
    bool cancelled = false;
    bool killed = false;
    Clock::time_point grace_deadline = started;
    // 【语法】static_cast<long long>(timeout_ms) * 1000：
    //         先把 int 扩宽再乘，避免 int 溢出（1000ms * 1000 = 1e6，int 够，
    //         但如果是 3e6 ms 就不够了）。
    const long long timeout_us = static_cast<long long>(options.timeout_ms) * 1000;

    // 主循环：一直抽管道，直到两根都 EOF（或者我们已经放弃等待）。
    while (!out.eof || !err.eof) {
        const auto now = Clock::now();

        if (!killed) {
            // 【语法】options.stop 可能为 nullptr，先判空再 load。
            //         memory_order_relaxed 只保证原子性，对「停止标志」足够。
            const bool stop_requested =
                options.stop != nullptr && options.stop->load(std::memory_order_relaxed);
            if (stop_requested) {
                cancelled = true;
            } else if (elapsed_us(started, now) >= timeout_us) {
                timed_out = true;
            }
            if (cancelled || timed_out) {
                // 对整个进程组动手，孙子一起带走。
                child.signal_group(SIGKILL);
                killed = true;
                // 【知识点】杀完之后还要继续抽管道，直到 EOF；但万一有孙子
                // 逃出了进程组、攥着写端不放，就给自己一个截止时间，
                // 到点直接放弃，不让它拖死整个程序。
                grace_deadline = now + std::chrono::milliseconds(kDrainGraceMs);
            }
        } else if (now >= grace_deadline) {
            break;  // 已被杀，但管道迟迟不 EOF：放弃等待，不让它拖死整个程序
        }

        // 【语法】栈数组 pollfd fds[2]，count 是实际用到的元素个数。
        //         如果某一根已经 EOF，就不再把它放进 poll 集合。
        pollfd fds[2];
        nfds_t count = 0;
        if (!out.eof) {
            fds[count].fd = out_read.get();
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
        }
        if (!err.eof) {
            fds[count].fd = err_read.get();
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
        }

        // 【API】poll(fds, count, 100)：
        //         >0 有 fd 就绪；0 超时；-1 出错。
        //         100ms 的分片让我们有机会回头检查超时和取消。
        const int rc = ::poll(fds, count, kPollSliceMs);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;  // 被信号打断：回去重新检查超时和取消
            }
            fail("poll");
        }
        if (rc == 0) {
            continue;  // 这一片没数据，回去处理超时
        }

        // 【语法】nfds_t 是 poll 用的无符号计数类型；这里按数组下标遍历。
        for (nfds_t i = 0; i < count; ++i) {
            if (fds[i].revents == 0) {
                continue;
            }
            // 【语法】根据 fd 反查是哪个 Channel。也可以把 Channel* 存进
            //         pollfd 旁边的数组，这里用「比较 fd」更直观。
            Channel& channel = (fds[i].fd == out_read.get()) ? out : err;
            drain_fd(channel);
        }
    }

    // 【规则】不管怎么结束，最后一定要 waitpid 回收，否则留下僵尸。
    result.status = child.reap();
    result.duration_us = elapsed_us(started, Clock::now());
    result.out_lines = out.lines;
    result.err_lines = err.lines;
    result.out_bytes = out.bytes;
    result.err_bytes = err.bytes;
    result.truncated = out.truncated || err.truncated;
    // 【语法】std::move(preview) 把 vector 的内容搬进结果，避免深拷贝。
    result.out_preview = std::move(out.preview);
    result.err_preview = std::move(err.preview);

    // 【规则】状态判定顺序很重要：
    //         先看「我们主动杀的吗」，再看「子进程自己怎么死的」。
    //         如果超时杀掉了它，status 会显示 signaled(SIGKILL)，
    //         但语义上应该报告 TimedOut。
    if (cancelled) {
        result.state = RunState::Cancelled;
    } else if (timed_out) {
        result.state = RunState::TimedOut;
    } else if (result.status.signaled) {
        result.state = RunState::Signaled;
    } else if (result.status.exited) {
        result.state = result.status.code == 0 ? RunState::Succeeded : RunState::Failed;
    } else {
        result.state = RunState::SpawnError;
    }
    return result;
}

}  // namespace jobrun
