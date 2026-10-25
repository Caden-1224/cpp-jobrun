// ============================================================================
//  process.hpp —— 真实子进程：创建、收割、以及把它的输出抓回来
//
//  这个模块是整份练习里最「操作系统」的部分：
//  fork / exec / pipe / dup2 / poll / waitpid / kill / 进程组 / 信号，
//  全部是真实系统调用，不是模拟。
//
// ============================================================================
//  【给只会 C 的你：这个文件在 C 里会长什么样】
// ============================================================================
//
//  C 的写法大概是这样：
//
//      struct capture_result {
//          int state;
//          int status;
//          int out_fd, err_fd;
//          size_t out_bytes, err_bytes;
//      };
//      int run_command(char *const argv[], struct capture_result *out);
//      // 调用方必须：检查返回值、接管 fd、记得 waitpid、记得 close。
//
//  问题不在「能不能写」，而在「谁负责什么」全靠约定。C++ 把责任写进类型：
//      * ChildProcess 拥有 pid，析构时兜底回收 → 不可能留僵尸
//      * Fd 拥有 fd，析构时 close → 不可能泄漏
//      * 结果按值返回，调用方不可能忘记初始化
//
// ============================================================================
//  【语法清单：本文件用到的 C++ 规则】
// ============================================================================
//
//  1. enum class
//         enum class RunState { Succeeded, Failed, ... };
//     作用域枚举：必须写 RunState::Succeeded，不会和别的枚举撞名；
//     也不会隐式转成 int（C 的 enum 会）。
//     【C 对照】C 的 enum 常量直接泄漏到外层作用域，而且可以和 int 混用。
//     可以指定底层类型：enum class RunState : unsigned char { ... };
//
//  2. 聚合体与默认成员初始化
//         struct ExitStatus {
//             bool reaped = false;
//             ...
//         };
//     没有用户声明的构造函数、没有 private 成员 → 它是聚合体，
//     可以用 ExitStatus s; 或 ExitStatus s{}; 初始化，未写的成员用默认值。
//     【C 对照】C 的 struct 没有成员默认值，ExitStatus s; 是未初始化垃圾。
//
//  3. std::optional<T>
//         std::optional<ExitStatus> try_reap();
//     「可能有一个 ExitStatus，也可能没有」。
//     【C 对照】C 里只能返回 int 当状态码 + 输出参数，或者返回 NULL 指针。
//     调用方用 if (auto s = child.try_reap()) { ... } 或 s.has_value()。
//
//  4. RAII 与 Rule of Five
//         ChildProcess 管一个 pid，析构时若未回收就强制回收。
//         拷贝删除、移动保留 —— 和 Fd 完全同构。
//
//  5. [[nodiscard]]
//         简单 getter 都加，避免「取了但没用」。
//
//  6. const 引用传参
//         const CaptureOptions& options
//     不拷贝、不允许修改、能绑定临时对象。
//     【C 对照】C 里传 const struct CaptureOptions *，调用方还得判空。
//
//  7. 返回结构体
//         CaptureResult run_command_captured(...);
//     按值返回一个大结构体，靠移动/RVO 避免拷贝。
//     【C 对照】C 里只能输出参数，或者返回 malloc 出来的指针（谁 free？）。
//
// ============================================================================
//  【本文件涉及的 POSIX API：参数、返回、errno、扩展】
// ============================================================================
//
//  pid_t fork(void);
//      返回：父进程拿到子进程 pid（> 0）；子进程拿到 0；失败 -1 + errno。
//      【关键】fork 返回两次，父子各一次。子进程是父进程的完整副本
//      （copy-on-write），连 fd 表、信号处置、进程组都复制一份。
//      【扩展】vfork（共享地址空间，危险）、posix_spawn（fork+exec 的封装，
//      更轻量）、clone（可精细控制共享哪些资源，线程就是 clone 出来的）。
//
//  int execvp(const char *file, char *const argv[]);
//      成功：不返回，进程映像被替换。
//      失败：返回 -1 + errno（ENOENT/EACCES/ENOEXEC）。
//      【规则】argv 必须以 NULL 结尾；argv[0] 是程序名。
//      【扩展】execv（不搜 PATH）、execve（自己给环境变量）、
//      execlp（可变参数）、fexecve（用 fd 执行）、execvpe（GNU 扩展）。
//
//  pid_t waitpid(pid_t pid, int *wstatus, int options);
//      pid    : >0 等指定子进程；-1 等任意子进程；0 等同进程组；
//               < -1 等进程组 |pid|。
//      options: 0 阻塞；WNOHANG 非阻塞；WUNTRACED/WCONTINUED 也报告停止/继续。
//      返回   : >0 被回收的子进程 pid；0（仅 WNOHANG）还没结束；-1 + errno。
//      【规则】回收后用 WIFEXITED/WEXITSTATUS、WIFSIGNALED/WTERMSIG 解码。
//      【扩展】waitid（更细的状态）、SIGCHLD 通知、pidfd（Linux 5.3+）。
//
//  int kill(pid_t pid, int sig);
//      pid > 0 : 发给该进程
//      pid == 0: 发给「同进程组的所有进程」
//      pid < 0 : 发给「进程组 |pid| 的所有进程」  ← 本项目的整组杀
//      pid == -1: 发给所有有权限的进程（危险）
//      返回 0 / -1 + errno（ESRCH 目标不存在、EPERM 权限不足）。
//      【扩展】pidfd_send_signal（避免 pid 复用的竞态）、tgkill、raise。
//
//  int setpgid(pid_t pid, pid_t pgid);
//      pid == 0 表示当前进程；pgid == 0 表示「用 pid 当组号」。
//      返回 0 / -1 + errno（EACCES、ESRCH、EPERM）。
//      【扩展】setsid（新会话）、getpgid、tcsetpgrp（前台进程组）。
//
//  int pipe2(int pipefd[2], int flags);   （见 fd.hpp）
//  int dup2(int oldfd, int newfd);
//      把 newfd 变成 oldfd 的副本；如果 newfd 已经打开，先原子地关闭它。
//      返回 newfd / -1 + errno；失败时 oldfd 不被关闭。
//      【规则】dup2 会清掉 newfd 上的 FD_CLOEXEC —— 这正是重定向能活过 exec 的原因。
//      【扩展】dup（返回最小可用 fd，不指定目标）、dup3（可指定 CLOEXEC）。
//
//  int poll(struct pollfd *fds, nfds_t nfds, int timeout);
//      fds    : 数组，每个元素 { fd, events, revents }
//      nfds   : 数组长度
//      timeout: 毫秒；-1 永远等；0 立即返回
//      返回   : >0 就绪的 fd 数；0 超时；-1 + errno（EINTR 可重试）
//      【扩展】epoll（Linux，海量 fd 更高效）、select（fd 上限 1024）、
//      ppoll（纳秒精度 + 原子信号掩码）、io_uring（异步 IO）。
// ============================================================================
#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "jobrun/fd.hpp"

namespace jobrun {

/// 一个作业最终走到了哪一步。
///
/// 【语法】enum class 的成员必须带作用域：RunState::Succeeded。
///         这让错误状态和别的枚举不会互相污染。
/// 【语法】enum class 默认底层类型是 int；可以显式写 : std::uint8_t 省空间。
enum class RunState {
    Succeeded,   ///< 正常退出，退出码 0
    Failed,      ///< 正常退出，但退出码非 0
    Signaled,    ///< 被信号终止（段错误、被 kill 等）
    TimedOut,    ///< 超过时限，被我们 SIGKILL 掉
    Cancelled,   ///< 用户按下 Ctrl-C，主动收工
    SpawnError,  ///< fork 或 exec 失败
};

/// 把 RunState 印成人看的字。
///
/// 【语法】const char* 返回静态字符串，不分配内存、不抛异常。
///         noexcept 在这里是诚实的：switch 覆盖了全部枚举值。
const char* to_string(RunState state) noexcept;

/// waitpid 交回来的状态，拆成「怎么结束的」。
///
/// 【语法】这是一个聚合体：bool/int/string 都能默认初始化。
/// 【C 对照】C 里 waitpid 给你一个 int status，你必须自己用 WIFEXITED 等宏解码；
///         这里把「解码后的语义」封装成一个结构体。
struct ExitStatus {
    bool reaped = false;
    bool exited = false;   ///< WIFEXITED
    bool signaled = false; ///< WIFSIGNALED
    int code = 0;          ///< 正常退出码
    int term_signal = 0;   ///< 终止它的信号号
    /// 【语法】const 成员函数 + 按值返回 string。
    ///         返回值优化（RVO）让这个 string 直接在调用方构造，没有多余拷贝。
    std::string describe() const;
};

/// 子进程句柄（RAII）。
///
/// 【知识点：为什么需要一个类】裸 pid_t 只是一个 int。谁负责回收它？
/// 忘了 waitpid 就留下僵尸进程（zombie）—— 进程已经死了，但内核里的
/// task_struct 还占着，因为退出码没人取走。把这个责任写进析构函数，
/// 「忘记回收」这件事就变得不可能发生。
///
/// 【C 对照】C 里你会写 struct child { pid_t pid; int reaped; };
///         再配 child_wait/child_kill，然后祈祷每个调用点都记得调。
class ChildProcess {
public:
    ChildProcess() noexcept = default;

    /// 【语法】explicit：不允许 pid_t 隐式变成 ChildProcess。
    /// 【规则】接管之后，这个对象就是 pid 的唯一负责人。
    explicit ChildProcess(pid_t pid) noexcept;

    /// 析构：如果还没回收，先杀再收，绝不留僵尸。
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    [[nodiscard]] bool valid() const noexcept { return pid_ > 0; }
    [[nodiscard]] bool reaped() const noexcept { return reaped_; }

    /// 向「整个进程组」发信号。
    ///
    /// 【知识点：为什么是 -pid 而不是 pid】子进程在启动时自成一个进程组，
    /// 组长就是它自己。kill(-pid) 会把信号发给组里所有成员 —— 包括
    /// 它 fork 出来的孙子进程。只杀直接子进程的话，
    /// `sh -c 'sleep 100'` 里的 sleep 会活下来变成孤儿，
    /// 继续占着它继承到的管道写端，于是父进程那边永远等不到 EOF。
    ///
    /// 【参数】sig 是信号编号，例如 SIGKILL / SIGTERM / SIGINT。
    void signal_group(int sig) const;

    /// 非阻塞回收。返回 nullopt 表示孩子还在跑。
    ///
    /// 【API】内部用 waitpid(pid_, &status, WNOHANG)：
    ///       返回 0  → 孩子还在跑，返回 nullopt
    ///       返回 pid → 已回收，解码状态后返回
    ///       返回 -1  → ECHILD 等，说明早被回收过，返回空 ExitStatus
    std::optional<ExitStatus> try_reap() noexcept;

    /// 阻塞回收。
    /// 【API】waitpid(pid_, &status, 0)，循环重试 EINTR。
    ExitStatus reap() noexcept;

private:
    // 【语法】类内成员初始化：默认构造出来就是一个「空句柄」。
    pid_t pid_ = -1;
    bool reaped_ = false;
};

/// 抓取选项。
///
/// 【语法】`= 5000` 这样的默认成员初始化，让 `CaptureOptions opt;` 得到一个
///         可直接使用的对象，而不是一堆未初始化的字节。
/// 【C 对照】C 里必须 memset 或逐个赋值，或者提供一个 capture_options_init()。
struct CaptureOptions {
    int timeout_ms = 5000;
    std::size_t max_preview_lines = 4;
    /// 停止标志。用户按 Ctrl-C 时被置位，正在跑的作业会被收掉。
    /// 【语法】指向 const 的指针：本函数只读不写。
    /// 【坑】调用方必须保证它指向的 atomic 在 run 期间一直活着。
    const std::atomic<bool>* stop = nullptr;
};

/// 一次命令执行的完整结果。
///
/// 【语法】std::vector<std::string> 会自己管理内存，所以这个结构体可以
///         安全地按值返回、按值存储、在离开作用域时自动释放。
/// 【C 对照】C 里返回这种结构体要么调用方提供缓冲区，要么 malloc + 谁 free 的约定。
struct CaptureResult {
    RunState state = RunState::SpawnError;
    ExitStatus status;
    long long duration_us = 0;   ///< 从 fork 到回收，微秒
    std::size_t out_lines = 0;
    std::size_t err_lines = 0;
    std::size_t out_bytes = 0;
    std::size_t err_bytes = 0;
    bool truncated = false;      ///< 是否有某一行太长被截断过
    int spawn_errno = 0;         ///< 仅 SpawnError 时有效：exec 失败时的 errno
    std::vector<std::string> out_preview;
    std::vector<std::string> err_preview;
};

/// fork + exec 一条命令，把 stdout/stderr 全部读回来，并完成回收。
///
/// 这个函数在做四件事，缺一不可：
///   1. 建两根管道，fork，子进程里 dup2 重定向后 exec；
///   2. 父进程立刻关掉两个写端（否则 read 永远等不到 EOF）；
///   3. 用 poll + 非阻塞 read 持续把管道抽干
///      （不抽的话，子进程写满 64KiB 管道缓冲区就会永久阻塞）；
///   4. 超时或取消时，向整个进程组发 SIGKILL，再 waitpid 回收。
///
/// 【参数】argv    : 命令 + 参数，argv[0] 是程序名；内部会补一个 nullptr。
///         options : 超时、样本行数、停止标志。
/// 【返回】CaptureResult：正常结束、非零退出、被信号杀、超时、exec 失败，
///         全都通过 state 字段区分。
/// 【异常】fork 失败、poll 失败会抛 std::system_error；子进程自己失败不算异常，
///         而是通过 CaptureResult::state 返回 —— 这是「错误」与「异常」的分界：
///         作业失败是正常业务，系统调用失败才是异常。
///
/// 【语法】const std::vector<std::string>&：只读引用，不拷贝 argv。
/// 【语法】按值返回 CaptureResult：C++17 保证拷贝省略，调用方直接构造，
///         连移动都不会发生。
CaptureResult run_command_captured(const std::vector<std::string>& argv, const CaptureOptions& options);

}  // namespace jobrun
