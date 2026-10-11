// ============================================================================
//  process.cpp —— 真实子进程：fork / exec / pipe / poll / waitpid / kill
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

using Clock = std::chrono::steady_clock;

/// 每次 poll 最多等这么久。分片轮询有两个好处：
/// 一是有机会检查「用户按了 Ctrl-C 吗」，二是有机会检查「超时了吗」。
/// 如果在这里写 -1（无限等），取消和超时就都失效了。
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

long long elapsed_us(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
}

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
    return s;
}

/// 兜底回收：析构、移动赋值都用它，保证绝不留僵尸。
void force_reap(pid_t pid) noexcept {
    if (pid <= 0) {
        return;
    }
    // 先礼后兵：SIGTERM 请它走，随即 SIGKILL 保证它走。
    ::kill(-pid, SIGTERM);
    ::kill(pid, SIGTERM);
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

/// 一根管道上的读取进度。
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
std::string clip_for_preview(const std::string& line) {
    if (line.size() <= kMaxPreviewChars) {
        return line;
    }
    std::size_t cut = kMaxPreviewChars;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return line.substr(0, cut) + " …（本行过长，样本已截断）";
}

/// 把 pending 里所有完整的行结算掉。
void harvest_lines(Channel& ch) {
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = ch.pending.find('\n', start);
        if (nl == std::string::npos) {
            break;
        }
        if (!ch.discarding) {
            if (ch.preview.size() < ch.preview_limit) {
                ch.preview.push_back(clip_for_preview(ch.pending.substr(start, nl - start)));
            }
            ++ch.lines;
        }
        ch.discarding = false;  // 无论之前是不是在丢弃，这一行的尾巴都找到了
        start = nl + 1;
    }
    if (start > 0) {
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
void drain_fd(Channel& ch) {
    char buf[kReadChunk];
    for (;;) {
        const ssize_t n = ::read(ch.fd->get(), buf, sizeof buf);
        if (n > 0) {
            ch.bytes += static_cast<std::size_t>(n);
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
        ch.eof = true;  // 其它错误：这根管子废了，不再等它
        return;
    }
}

}  // namespace

const char* to_string(RunState state) noexcept {
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

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : pid_(other.pid_), reaped_(other.reaped_) {
    other.pid_ = -1;
    other.reaped_ = true;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
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
    if (errno == ESRCH) {
        ::kill(pid_, sig);
    }
}

std::optional<ExitStatus> ChildProcess::try_reap() noexcept {
    if (pid_ <= 0 || reaped_) {
        return std::nullopt;
    }
    int status = 0;
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
        throw std::invalid_argument("run_command_captured: argv 为空");
    }

    // ---- fork 之前，把所有会分配内存的事情做完 ----
    // 【知识点：fork 之后不能碰 malloc】fork 只复制调用它的那个线程，
    // 但其它线程手里的锁全都被复制成「已加锁」状态，且永远不会有人来解锁。
    // 所以在子进程里 new / std::string / iostream / printf 都可能死锁。
    // 结论：argv 必须在 fork 之前就转成 char* 数组。
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
        raw.push_back(const_cast<char*>(arg.c_str()));
    }
    raw.push_back(nullptr);

    auto [out_read, out_write] = Fd::make_pipe();
    auto [err_read, err_write] = Fd::make_pipe();
    // 第三根管道专门用来回答一个问题：exec 到底成功了没有？
    //
    // 【知识点：CLOEXEC 的巧妙用法】它的写端带 O_CLOEXEC。
    //   * exec 成功 -> 内核在切换映像时自动关掉这个写端 -> 父进程读到 EOF(0 字节)
    //   * exec 失败 -> 子进程抢在自己 _exit 之前把 errno 写进去 -> 父进程读到 >0 字节
    // 于是「读到 0 字节」这件事本身，就成了 exec 成功的证据。
    // 这比「用退出码 127 代表 command not found」可靠得多。
    auto [exec_read, exec_write] = Fd::make_pipe();

    const auto started = Clock::now();

    const pid_t pid = ::fork();
    if (pid < 0) {
        fail("fork");
    }

    if (pid == 0) {
        // ============================== 子进程 ==============================
        // 从这里到 execvp 之间，只能调用 async-signal-safe 的函数。

        // 1. 自成一个进程组，组长是自己。
        ::setpgid(0, 0);

        // 2. 把被父进程改过的信号处置恢复成默认。
        // 【知识点：信号处置会被 fork 继承】如果父进程把 SIGINT 设成了
        // 「忽略」，子进程会原样继承这个「忽略」，于是 Ctrl-C 对它毫无作用。
        // 我们必须显式恢复，子进程才回到正常的默认行为。
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGPIPE, SIG_DFL);

        // 3. 重定向标准输出与标准错误。
        // 【知识点：dup2 会清掉新 fd 上的 CLOEXEC】正因为如此，
        // 我们这两根管道才能活过 exec；而其它所有 fd 都带 CLOEXEC，
        // exec 时被内核顺手关掉 —— 包括别的作业的管道。
        ::dup2(out_write.get(), STDOUT_FILENO);
        ::dup2(err_write.get(), STDERR_FILENO);

        // 4. 换程序。
        ::execvp(raw[0], raw.data());

        // 只有 exec 失败才会执行到这里。
        const int exec_errno = errno;
        ssize_t ignored = ::write(exec_write.get(), &exec_errno, sizeof(exec_errno));
        (void)ignored;
        // 【知识点：_exit 而不是 exit】exit() 会跑 atexit 处理器、并 flush
        // 继承自父进程的 stdio 缓冲区 —— 那会把父进程攒着还没打印的内容
        // 在这里重复打印一遍。_exit 直接进内核，什么都不跑。
        ::_exit(127);
    }

    // ============================== 父进程 ==============================

    // 与子进程里的 setpgid 构成「双向竞赛」：两边都设置一遍，
    // 无论谁先跑到，进程组都已经就绪，不存在竞态窗口。
    ::setpgid(pid, pid);

    // 【知识点：必须关掉写端】管道的内核缓冲区由「还有没有写端」决定生死。
    // 父进程如果攥着写端不放，即使子进程早就退出，read() 也永远等不到 EOF。
    out_write.reset();
    err_write.reset();
    exec_write.reset();  // 不关这个，上面那个「EOF 即成功」的技巧就不成立了

    ChildProcess child(pid);

    // 等 exec 的结论。这一步不会挂住：exec 要么立刻成功（EOF），
    // 要么立刻失败（写回 errno）。
    CaptureResult result;
    int spawn_errno = 0;
    {
        int value = 0;
        ssize_t n = 0;
        do {
            n = ::read(exec_read.get(), &value, sizeof(value));
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            spawn_errno = value;
        }
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
    const long long timeout_us = static_cast<long long>(options.timeout_ms) * 1000;

    // 主循环：一直抽管道，直到两根都 EOF（或者我们已经放弃等待）。
    while (!out.eof || !err.eof) {
        const auto now = Clock::now();

        if (!killed) {
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
                grace_deadline = now + std::chrono::milliseconds(kDrainGraceMs);
            }
        } else if (now >= grace_deadline) {
            break;  // 已被杀，但管道迟迟不 EOF：放弃等待，不让它拖死整个程序
        }

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

        for (nfds_t i = 0; i < count; ++i) {
            if (fds[i].revents == 0) {
                continue;
            }
            Channel& channel = (fds[i].fd == out_read.get()) ? out : err;
            drain_fd(channel);
        }
    }

    result.status = child.reap();
    result.duration_us = elapsed_us(started, Clock::now());
    result.out_lines = out.lines;
    result.err_lines = err.lines;
    result.out_bytes = out.bytes;
    result.err_bytes = err.bytes;
    result.truncated = out.truncated || err.truncated;
    result.out_preview = std::move(out.preview);
    result.err_preview = std::move(err.preview);

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
