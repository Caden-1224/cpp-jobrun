# cpp-jobrun

**速成之后缺的那一次完整练习：一个真进程、真线程、真信号的并发作业执行器。**

![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![CMake](https://img.shields.io/badge/build-CMake-064F8C)
![dependencies](https://img.shields.io/badge/dependencies-0%20(only%20pthread)-brightgreen)
![license](https://img.shields.io/badge/license-MIT-blue)
![lines](https://img.shields.io/badge/lines-2220%20%2F%20394%20comments-lightgrey)

零第三方依赖 · 只用标准库 + POSIX · 自带自检 · 每个知识点都在代码里标了出来

---

## 一、这个故事接着上一份练习

如果你写过 [rust-sched-sim](https://github.com/Caden-1224/rust-sched-sim)，你会记得它的一句话总结：**那是一个模型**——假任务、假设备、单线程，所有"并发"都是假装出来的。

第二次速成 C++ 之后，我发现同样的位置又空了一次，但**洞的形状不一样**：

- Rust 速成班留下的洞是「所有权懂了，但没用过」
- **C++ 速成班留下的洞是另一个**：你学会了语法，却不知道一个程序**是怎么被操作系统跑起来的**

你的 C++ 速成大概长这样：

- 第 1 天：`cout`、`vector`、`string`、类、`public/private` —— 好像就是加强版 C
- 第 2 天：指针、引用、`new/delete`、头文件 —— 开始有点难受
- 第 3 天：模板、STL、lambda、`std::thread` —— 每个词都见过，但没人告诉你它们为什么存在
- 然后你 `g++ main.cpp -o a.out`，跑出结果，**但你从来没写过一个由多个 `.cpp` 组成、需要真正的链接器、会 `fork`、会开线程、会收信号的程序**

教程里的例子也救不了场：它们全都活在一个 `main` 里，没有头文件，没有 CMake，没有 `fork`，没有僵尸进程，没有管道。你学的那一整套东西，根本没有出场的机会。

**这个仓库就是中间那段空白，而且它比上一版更狠：这一版里没有任何东西是模拟的。**

---

## 二、它是什么

一个**并发的多进程作业执行器**。你给它一批命令，它用 N 个线程并发地把它们跑起来，每条命令都**真的 `fork` 出一个进程**，把 stdout/stderr **真的用管道抓回来**，超时了**真的整组杀掉**，最后出一张作业表。

```console
$ ./build/jobrun

本次作业清单（共 11 个）
  #0  echo-fast           [进程]  sh -c echo hello from jobrun
  #1  stream-lines        [进程]  sh -c for i in 1 2 3 4 5; do echo "逐行输出 line $i"; ...
  #4  big-stdout          [进程]  sh -c seq 1 200000
  #7  timeout-trap        [进程]  sh -c sleep 30; echo 这一行永远不该出现
  #10 builtin-sum         [进程内] <进程内> builtin-sum
  ...

以 4 个 worker 启动，待跑队列容量 16，单作业时限 1000 ms
  ▶ # 0 echo-fast 开跑
  ▶ # 1 stream-lines 开跑
  ▶ # 2 exit-nonzero 开跑
  ▶ # 3 killed-by-signal 开跑
  ✔ # 0 echo-fast —— 成功，退出码 0（0.6 ms）
  ▶ # 4 big-stdout 开跑
  ✘ # 2 exit-nonzero —— 失败，退出码 3（1.0 ms）
  ☠ # 3 killed-by-signal —— 被信号终止，被信号 15 终止（1.0 ms）
  ...
  ✔ # 1 stream-lines —— 成功，退出码 0（256.2 ms）
  ⚠ # 9 missing-binary —— 启动失败，无法启动：No such file or directory（0.2 ms）
  ⏱ # 7 timeout-trap —— 超时，超过 1000 ms，已终止整个进程组（1004.0 ms）

============================== 作业档案（按提交顺序） ==============================
  #  作业                执行位置  结局               排队         运行         stdout    stderr
--------------------------------------------------------------------------------------------
  0  echo-fast           进程      成功             0.0 ms       0.6 ms    1 行 / 18 B      0 行
  1  stream-lines        进程      成功             0.0 ms     256.2 ms   5 行 / 100 B      0 行
  2  exit-nonzero        进程      失败             0.0 ms       1.0 ms    1 行 / 25 B      0 行
  3  killed-by-signal    进程      被信号终止       0.0 ms       1.0 ms     0 行 / 0 B      0 行
  4  big-stdout          进程      成功             0.6 ms       3.3 ms  200000 行 / 1258.7 KiB      0 行
  7  timeout-trap        进程      超时             1.6 ms    1004.0 ms     0 行 / 0 B      0 行
  8  long-single-line    进程      成功             1.7 ms       1.2 ms  1 行 / 195.3 KiB      0 行
  9  missing-binary      进程      启动失败         3.0 ms       0.2 ms     0 行 / 0 B      0 行
 10  builtin-sum         进程内    成功             3.2 ms       0.0 ms    2 行 / 57 B      0 行
  ...

================================ 统计 ================================
总作业数 11（开进程 10，进程内 1）
成功 7，失败 1，被信号终止 1，超时 1，已取消 0，启动失败 1
抓回 stdout 200012 行 / 1454.2 KiB，stderr 1 行 / 20 B
整批墙钟 1103.6 ms，所有作业耗时之和 1268.9 ms，并发度 4，加速比 1.15×
```

**上面这些数字全部来自真实系统调用。** `#4` 那 200000 行、1258.7 KiB 是真的从管道里一字节一字节读回来的；`#7` 那个 `sleep 30` 是真的被 `SIGKILL` 掉的；`#9` 的 `No such file or directory` 是从 `execvp` 的 `errno` 里拿到的。

（这是一次真实运行的抓取。耗时和调度顺序每次都会变 —— 那正是"过程不确定"；但那张按提交序号排好的档案表永远一致。）

---

## 三、为什么这一版必须写"真的"

因为**并发这东西没法模拟**。

Rust 那版可以模拟调度，因为调度本质上是"数据 + 规则"。但 C++ 这版要讲的东西，全都只有在真实环境里才会露出本来面目：

- **管道有缓冲区**（Linux 默认 64 KiB）。子进程写满了就阻塞。不持续读，它永远不结束 —— 这件事你只能在真的管道上撞到。
- **`fork` 在多线程程序里是危险的**。它只复制调用它的那个线程，别的线程手里的锁全都变成"永远不解锁"。这件事你只能在真的有线程的时候撞到。
- **信号处置会被 `fork` 继承**。父进程忽略了 `SIGINT`，子进程也忽略，于是 Ctrl-C 对它无效。
- **`kill` 只杀一个进程，杀不掉它的子孙**。`sh -c 'sleep 30'` 里的 `sleep` 会活下来，继续攥着继承到的管道写端。
- **fd 会泄漏，进程会变僵尸** —— 而且这两件事在"跑一次就退出"的程序里**根本测不出来**。

所以这一版里，`Fd`、`ChildProcess` 这些类不是设计模式练习，它们是**用来让上面这些事故不可能发生的装置**。

---

## 四、你会在这里用到哪些 C++ 知识

这次是真的"一次性囊括"——速成班讲过的每个概念都有一处**非它不可**的落点：

| 知识点 | 在代码里 | 为什么这里绕不开 |
| --- | --- | --- |
| 头文件 / 分离编译 / 编译单元 | `include/` + `src/` | 7 个模块各自编译成 `.o`，靠链接器拼起来 |
| 声明与定义分离 | 每个 `.hpp` / `.cpp` 对 | 头文件只写"有什么"，实现在 `.cpp` |
| `#pragma once` / ODR | 所有头文件 | 同一实体全程序只能定义一次，否则链接报重定义 |
| 命名空间 | `namespace jobrun` | 把名字关起来，不和别人撞 |
| **RAII** | `Fd` / `ChildProcess` / `SignalWatcher` | 析构即 `close` / 即 `waitpid`：把"不能忘的事"交给编译器 |
| **移动语义** | `Fd::Fd(Fd&&)`、`std::move` | 管道只能有一个主人，"搬走"和"复制"是两件事 |
| **Rule of Five** | `Fd` 删掉拷贝、自己写移动 | 默认拷贝会造成两次 `close`，第二次关掉的可能是别人的 fd |
| 继承 + 虚函数 + 纯虚接口 | `Job` → `ShellJob` / `BuiltinJob` | "怎么执行"是变化点，`run()` 交给子类 |
| 虚析构 | `virtual ~Job() = default` | 通过 `Job*` 删除子类，析构不虚就泄漏 |
| 智能指针 | `unique_ptr<Job>` | 多态持有 + 唯一所有权，`std::move` 明确表达"交出去了" |
| 模板 | `BoundedQueue<T>`（头文件即实现） | 元素类型由使用处决定，所以实现必须能被看到 |
| `std::function` | `BuiltinJob::Body` | 类型擦除：lambda、函数指针、绑定过的成员函数都能装 |
| 异常 | `std::system_error(errno)`、`try/catch` | 系统调用失败抛异常；作业失败不是异常，是数据 |
| 异常安全 | `ChildProcess` 析构兜底 | 中途抛异常也不会留下僵尸 |
| `const` 正确性 / `mutable` | 全篇；`BoundedQueue::mutex_` | `const` 方法要加锁，所以 mutex 必须 mutable |
| `operator<<` 重载 | `RunState` / `RunStats` | 自定义类型也能像内置类型一样被打印 |
| lambda 与捕获 | 参数解析、worker、`make_default_jobs` | 按引用捕获让 `++i` 影响外层；按值捕获独立存起来 |
| STL | `vector` / `deque` / `optional` / `string` / `chrono` | 各司其职，不是为了用而用 |
| `<algorithm>` | `std::sort` 按提交序号重排 | 并发结果要有确定顺序 |
| **`std::thread`** | `Runner` 的 worker 线程池 | 并发执行 |
| **`std::mutex` / `lock_guard` / `unique_lock`** | 结果收集、日志、队列 | `lock_guard` 够用的地方别用 `unique_lock` |
| **`std::condition_variable`** | `BoundedQueue` 的两个条件变量 | 生产者消费者的核心；必须用带谓词的 `wait` |
| **`std::atomic<bool>`** | 全局停止标志 | 一个线程置位，所有线程立刻看见 |
| CMake 目标与依赖传播 | `jobrun_core` 静态库 | 两个可执行文件共用一份代码；`PUBLIC` vs `PRIVATE` |

---

## 五、你会顺手学到哪些操作系统概念

| 概念 | 在这个项目里 |
| --- | --- |
| 进程创建与程序替换 | `fork` / `execvp`，以及为什么 `exec` 之后代码就换了一个 |
| 进程组 | `setpgid(0, 0)` 让每个作业自成一组建制，于是可以整组下手 |
| 进程回收与僵尸 | `waitpid`；为什么"忘了 wait"会留下僵尸；RAII 如何让它不可能发生 |
| 管道与文件描述符语义 | 为什么必须关掉写端才能等到 EOF；为什么 `O_CLOEXEC` 是刚需 |
| 重定向 | `dup2` 把管道接到子进程的 fd 1 / fd 2 上 |
| I/O 多路复用 | `poll` + 非阻塞 `read`，一次盯两根管道 |
| 阻塞与非阻塞 | 阻塞 fd 上的"读到 EAGAIN"循环 vs `poll` 分片 |
| 信号 | `sigaction`、处置的继承、以及 self-pipe trick |
| 信号与进程组 | `kill(-pgid, sig)` 与单杀一个进程的区别 |
| 管道缓冲区与背压 | 64 KiB 缓冲、必须持续抽干、有界队列形成的背压 |
| 线程与同步 | 线程池、互斥、条件变量、优雅关闭 |
| 生产者消费者 | `BoundedQueue<T>` 就是课本上那个有界缓冲区 |
| 并发的不确定性 | 开始/结束顺序每次都不同，但报告永远一致 |
| 文件描述符作为资源 | 泄漏、上限、以及 RAII 的治理 |

---

## 六、快速开始

```bash
git clone git@github.com:Caden-1224/cpp-jobrun.git
cd cpp-jobrun

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # 生成构建系统
cmake --build build -j                            # 编译
./build/jobrun                                    # 跑
```

只依赖一个 C++17 编译器和 CMake 3.16+。没有任何第三方库。

跑完之后会写出 `jobrun-report.txt`（和屏幕上是同一份报告）。

### 先试这三个参数

```bash
./build/jobrun -j 1        # 串行跑一遍，看墙钟时间，对比加速比
./build/jobrun -j 8        # 并发拉满，看加速比涨到多少
./build/jobrun --timeout 200   # 时限收紧，看有多少作业掉进超时那条路
```

**注意退出码是 1**，因为默认清单里有几个**故意失败**的作业。这是对的：作业失败就该让调用方知道。被 Ctrl-C 打断时退出码是 130（128 + SIGINT）。

### 代码地图

| 文件 | 行数 | 负责什么 |
| --- | --- | --- |
| `include/jobrun/fd.hpp` + `src/fd.cpp` | 69 + 122 | **先读这个**：RAII 与移动语义的最小完整例子 |
| `include/jobrun/process.hpp` + `src/process.cpp` | 123 + 498 | 全项目的心脏：fork / exec / pipe / poll / waitpid / 进程组 |
| `include/jobrun/job.hpp` + `src/job.cpp` | 106 + 182 | `Job` 抽象基类、两种子类、默认作业清单 |
| `include/jobrun/queue.hpp` | 106 | `BoundedQueue<T>`：模板 + 条件变量 + 优雅关闭 |
| `include/jobrun/signals.hpp` + `src/signals.cpp` | 41 + 113 | self-pipe trick：把信号安全地送进主循环 |
| `include/jobrun/runner.hpp` + `src/runner.cpp` | 67 + 207 | 线程池、信号观察线程、按序号重排 |
| `include/jobrun/report.hpp` + `src/report.cpp` | 58 + 216 | `operator<<`、统计、UTF-8 宽度对齐的表格 |
| `src/main.cpp` | 166 | 参数解析、文件写出、退出码 |
| `tests/selfcheck.cpp` | 146 | 自检：反复跑，查 fd 泄漏 / 僵尸 / 分类 |
| `CMakeLists.txt` | 84 | 静态库 + 两个可执行目标 |

---

## 七、五条练习路线

### Level 0 · 跑起来改参数（约 30 分钟）

- `-j 1` 和 `-j 8` 各跑一次，对比"整批墙钟"和"加速比"两行
- 把 `--timeout` 从 `1000` 调到 `200`，观察哪些作业从"成功"变成"超时"
- 跑的时候开另一个终端 `ps -eo pid,ppid,pgid,stat,comm`，亲眼看看 `sh` 和它的 `sleep` 孙子**是不是在同一个进程组里**（这是整组 kill 能生效的原因）
- 跑完再 `ps aux | grep sleep`，确认没有孤儿

> **过关标准**：你能不看代码，说出"我给的那个数"是通过哪条路径影响最终输出的。

### Level 1 · 改（约 1–2 小时）

- **加一个作业**：在 `make_default_jobs()` 里加一条。先回答：它该是 `ShellJob` 还是 `BuiltinJob`？
- **加一种作业**：写 `class HttpJob : public Job`（或任何你能想到的），实现三个纯虚函数。**上层的线程池、报告、统计一行都不用改** —— 这就是多态的意义
- **让输出不截断**：`kMaxPreviewChars` 调大，观察报告变难看；理解"样本"和"数据"的区别
- **换掉整组 kill**：把 `signal_group(SIGKILL)` 改成只杀直接子进程，然后跑一次，看 `sleep 30` 是不是变成了孤儿（**跑完记得杀掉它**）

### Level 2 · 写（约半天）

- **作业依赖**：给 `Job` 加 `std::vector<std::size_t> deps`，只有依赖全部成功才能开工。这就从"并发执行"走到"DAG 调度"了
- **失败重试**：给 `RunnerConfig` 加 `max_retries`，失败的作业重新入队，并记录重试次数
- **`--strict`**：加一个开关，让"有任何作业失败"时退出码为 2，把作业失败和系统错误分开
- **资源限额**：在子进程 `exec` 之前调用 `setrlimit(RLIMIT_CPU, ...)`，让一个死循环的作业被内核而不是被我们的超时杀掉

### Level 3 · 挑战

- **用 `signalfd` 替掉 self-pipe**：Linux 专有，但更干净 —— 信号变成一个可以直接 `poll` 的 fd
- **用 `pidfd_open` + `pidfd_send_signal` 替掉 `kill(-pid)`**：Linux 5.3+。它解决了 `kill` 的一个经典竞态 —— pid 会被复用，你以为在杀 A，其实杀到了刚接替 A 的 B
- **换掉线程池模型**：用单线程 `epoll` 事件循环管理所有子进程的管道，对比两种模型在高并发下的表现
- **让并发度自适应**：不要固定 `-j N`，而是根据 CPU 核数和作业类型动态调整
- **加一面"背压仪表盘"**：实时打印队列深度，观察 `--queue` 调小之后生产者是怎么被拦住的

---

## 八、"为什么非得这么写"

这是这份练习里我最想让你带走的一节。

**代码里没有留下任何 bug** —— 留坑等于教错，初学者的第一反应是复制。但它们每一处都对应一个**真实的坑**，我把坑写在注释和这里，把正确写法写在代码里。

下面每一条，都是这个项目里**真的会出事**的地方：

| 写法 | 如果不这么写 |
| --- | --- |
| 每个 fd 都带 `O_CLOEXEC` | 并发 spawn 时，A 的子进程会继承到 B 的管道写端。B 退出后那个写端还开着，**B 的 worker 永远等不到 EOF，永久挂死** |
| 父进程 fork 后立刻关掉写端 | `read()` 永远等不到 EOF，因为管道还有写端开着（你自己） |
| 持续抽干管道，不"先等它结束再读" | 管道缓冲区只有 64 KiB。`seq 1 200000` 输出约 1.2 MiB，子进程写满就阻塞，而你在等它结束 —— **互相等，死锁** |
| `fork` 之前把 argv 转成 `char*` 数组 | `fork` 只复制调用它的那个线程，其它线程的锁在子进程里永远是"已加锁"。在子进程里 `new` / `std::string` / `printf` 都可能**死锁** |
| 子进程用 `_exit` 而不是 `exit` | `exit()` 会 flush 继承自父进程的 stdio 缓冲区，把父进程攒着还没打印的输出**重复打印一遍** |
| 子进程显式恢复 `SIGINT` 为 `SIG_DFL` | 父进程忽略 `SIGINT` 的话，子进程**原样继承"忽略"**，于是 Ctrl-C 对它毫无作用 |
| 超时/取消时 `kill(-pid)` 而不是 `kill(pid)` | 实测：`sh` 的 PGID 是它自己，它 fork 的 `sleep` **同组**。只杀 `sh`，`sleep` 会活下来变成孤儿，还攥着管道写端 |
| 用 CLOEXEC 探测管道判断 `exec` 成败 | 靠"退出码 127 代表找不到命令"不可靠 —— 程序自己也可能 `exit(127)`。探测管道是 **EOF = 成功**，语义精确 |
| `close()` 失败**不重试** | 在 Linux 上 `close()` 即使返回 `EINTR`，fd 也已经释放了。照着老习惯重试，**关掉的很可能是另一个线程刚打开的同号 fd** |
| `condition_variable::wait` 一定带谓词 | 虚假唤醒随时会发生；而且 `notify` 可能在你重新拿锁之前就发生，那个信号**直接丢了** |
| 关闭队列时用 `notify_all` | 只唤醒一个的话，其余消费者永远睡在 `not_empty_` 上，**`join()` 永久挂住，程序退不出去** |
| 先 `close()` 队列再 `join()` 线程 | 反过来先 `join`，worker 永远等不到新元素，`join` 也永远不返回 |
| worker 循环里分片 `poll`（100 ms）而不是 `poll(-1)` | 无限等的话，"检查超时"和"检查 Ctrl-C"这两件事**永远不会被执行** |
| 结果按提交序号 `std::sort` 重排 | 报告每次都不一样，没法对照、没法测试。"过程不确定，结论确定" |
| 信号处理器里只调 `write()` | 处理器可能打断 `malloc` 的临界区。在里面 `printf` / 加锁 = **随机死锁** |
| 输出样本按**显示长度**截断 | （我写这版时真踩到了）原本只截行数，结果 200 KB 的单行样本**把整个屏幕刷爆** |

### 一条真实的调试记录

第一版跑出来时，`missing-binary` 这个作业报"启动失败"，耗时却是 **340 ms**。一个注定失败的 `exec` 不该花这么久。

我先怀疑是 `fork` 慢，又怀疑是并发下的调度饥饿 —— 都不对（这台机器 24 核）。最后单独量了一下 `execvp`：

```console
PATH 条目数 = 80
裸名字(搜 PATH)      : 239.8 ms
绝对路径(不搜 PATH)  :   3.4 ms
存在的命令 /bin/true :   2.0 ms
```

原因找到了：`execvp` 会**逐个目录去搜 `PATH`**，而这台 WSL 的 `PATH` 有 80 项、一半挂在 Windows 盘上。每次 `stat` 都要跨一次 WSL 的 9p 边界，一次注定失败的查找就这么花掉了 240 ms。

代码没错，是环境的变量淹没了教学意图。所以那个作业改用了绝对路径，并把这段查证过程写进了注释里 —— **"先怀疑自己的代码，再怀疑环境，最后用数据把两者分开"**，这比任何语法知识都更值得带走。

---

## 九、自检

这个仓库带一个自检，`ctest` 一行就能跑：

```bash
ctest --test-dir build --output-on-failure
# 或直接跑
./build/selfcheck
```

```console
$ ./build/selfcheck
jobrun 自检
  [通过] 跑了 6 轮之后 fd 数量没变（4 -> 4），没有 fd 泄漏
  [通过] 一个子进程都没剩下，没有僵尸进程
  [通过] 每轮都提交了同样多的作业（11 个）
  [通过] 成功 7 个（实到 7）
  [通过] 失败 1 个（实到 1）
  [通过] 被信号终止 1 个（实到 1）
  [通过] 超时 1 个（实到 1）
  [通过] 启动失败 1 个（实到 1）
  [通过] 没有被取消的作业（实到 0）

全部通过。
```

它做的事很简单：在**同一个进程里**把同一批作业反复跑 6 轮，然后查三本账。

为什么必须"在同一个进程里反复跑"？因为 **fd 泄漏和僵尸进程在"跑一次就退出"的程序里根本看不出来** —— 进程一退，内核把所有资源都收回了，泄漏得再厉害也是干净的。只有在一个长命进程里反复跑，账才会显形。这是"跑一遍看看"和"测试"的根本差别。

---

## 十、这个仓库不是什么

- **不是一个生产级的作业调度系统**。没有沙箱、没有资源隔离（cgroup）、没有持久化、没有网络。它是**一份练习**
- **不是 C++ 语法教程**。语法不解释，只解释**取舍**。看不懂 `std::move` 的话，请先回去补速成班
- **不是跨平台的**。`fork`、`poll`、`setpgid` 都是 POSIX，只能跑在 Linux/macOS/WSL 上。Windows 原生需要另一套（`CreateProcess` + IOCP）
- **不是一份"标准答案"**。第七节那些 Level 3 的题目，我自己也没全做

它的大小是刻意的：**2220 行里有 394 行是纯注释，小到一个下午能读完，大到能让你亲手摸到进程、线程、管道、信号这四样东西。**

---

## 环境

- **C++17** 编译器（实测 g++ 11.4）
- **CMake 3.16+**（实测 3.22）
- Linux / macOS / WSL。用到 `pipe2`、`poll`、`setpgid`、`sigaction` 等 POSIX 接口
- 无第三方依赖（只链接 pthread，`std::thread` 的底层）

## 许可

MIT，见 [LICENSE](LICENSE)。随意 fork、改写、拿去当教学材料。

## 最后

上一版（Rust）教你**用数据结构和规则去建模一个系统**；这一版教你**让系统真的跑起来，然后收拾它留下的一切**。

两份练习合起来，是"从课本到机器"的一条完整路径。如果它们帮你跨过了速成之后那段空白，目的就达到了。有更好的题目，欢迎开 issue 聊聊。
