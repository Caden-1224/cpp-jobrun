# ============================================================================
#  CMakeLists.txt —— jobrun 的构建描述
#
# ============================================================================
#  【这个文件教什么】
# ============================================================================
#  * CMake 的「目标（target）」模型：库、可执行文件、测试
#  * target_* 命令：把编译选项、头文件路径、链接库挂到目标上
#  * PUBLIC / PRIVATE / INTERFACE 的区别
#  * 为什么多文件 C++ 需要一个构建系统
#
# ============================================================================
#  【给只会 C 的你：gcc 命令行 vs CMake】
#  ============================================================================
#  单文件时你只需要：
#      g++ -std=c++17 -O2 -Wall -Wextra main.cpp -o jobrun
#
#  多文件时你要手动做这些事：
#      g++ -std=c++17 -O2 -Iinclude -c src/fd.cpp -o fd.o
#      g++ -std=c++17 -O2 -Iinclude -c src/process.cpp -o process.o
#      ...（每个 .cpp 一次）
#      ar rcs libjobrun_core.a fd.o process.o ...
#      g++ main.o -L. -ljobrun_core -lpthread -o jobrun
#      g++ tests/selfcheck.cpp -L. -ljobrun_core -lpthread -o selfcheck
#
#  问题：
#    * 改一个头文件，哪些 .o 需要重编？手工判断迟早出错；
#    * 编译选项要抄很多遍，漏一个就是「我这能跑你那不能」；
#    * 换平台（Linux/macOS）时链接方式不同。
#
#  CMake 让你只描述「有哪些目标、每个目标依赖什么」，由它生成
#  Makefile / Ninja / Xcode 工程，并负责增量构建。
#
# ============================================================================
#  【语法清单：本文件用到的 CMake 规则】
#  ============================================================================
#  1. 命令名(参数...) 大小写不敏感，但习惯上用小写；变量区分大小写。
#  2. # 开始的行是注释（和 Python 一样）。
#  3. ${VAR} 引用变量；CMake 3.12+ 也支持 $<...> 生成器表达式。
#  4. set(VAR value) 定义普通变量；set(VAR value CACHE STRING "doc" FORCE)
#     定义缓存变量（可以用 -DVAR=value 从命令行覆盖）。
#  5. add_library(name STATIC files...) 定义静态库目标；
#     STATIC 生成 .a，SHARED 生成 .so，INTERFACE 不生成文件只传播属性。
#  6. add_executable(name files...) 定义可执行文件目标。
#  7. target_include_directories(target PUBLIC dir)：
#     给目标加头文件搜索路径。PUBLIC 表示「我自己用，链接我的人也用」。
#  8. target_link_libraries(target PRIVATE lib)：
#     给目标加链接依赖。PRIVATE 表示「只有我自己需要」。
#  9. foreach(var IN LISTS list) / endforeach()：循环。
# 10. if(...) / else() / endif()：条件；字符串比较用 STREQUAL，正则用 MATCHES。
#
# ============================================================================
#  【扩展：工程化还可以加什么】
#  ============================================================================
#  * target_compile_features(jobrun_core PUBLIC cxx_std_17)：用「特性」而不是
#    「标准版本号」表达要求，跨编译器更稳。
#  * set(CMAKE_EXPORT_COMPILE_COMMANDS ON)：生成 compile_commands.json，
#    clangd / IDE / clang-tidy 都靠它。
#  * target_precompile_headers：预编译头，加快大项目编译。
#  * FetchContent / find_package：引入第三方库。
#  * 加 sanitizer 选项：
#        option(JOBRUN_SANITIZE "..." OFF)
#        if(JOBRUN_SANITIZE)
#            target_compile_options(jobrun_core PRIVATE -fsanitize=thread)
#            target_link_options(jobrun_core PRIVATE -fsanitize=thread)
#        endif()
#  * ctest + add_test：本项目已经用了；CI 里跑 `ctest --output-on-failure`。
#  * ccache / sccache：编译缓存，CI 和本地都能加速。
# ============================================================================

# 【语法】cmake_minimum_required：声明这个工程需要的最低 CMake 版本。
#         低于它就直接报错，避免「用旧版 CMake 跑出奇怪结果」。
cmake_minimum_required(VERSION 3.16)

# 【语法】project(名字 VERSION ... DESCRIPTION ... LANGUAGES CXX)
#         定义工程名、版本、描述、启用的语言。只写 CXX 表示不需要 C 编译器。
project(jobrun
    VERSION 1.0.0
    DESCRIPTION "并发的多进程作业执行器：一份给 C++ 速成者的操作系统练习"
    LANGUAGES CXX)

# ---- 语言标准 ----
# 【语法】set(VAR value) 定义变量。CMAKE_CXX_STANDARD 是 CMake 认识的
#         特殊变量，会被翻译成 -std=c++17。
set(CMAKE_CXX_STANDARD 17)
# 【规则】REQUIRED ON：编译器不支持 C++17 就直接报错，而不是悄悄降级。
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 【知识点：CXX_EXTENSIONS OFF】关掉编译器私有扩展，
# 用 -std=c++17 而不是 -std=gnu++17。想要可移植就别依赖方言。
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 默认构建类型 ----
# 不指定就按 Release 构建：Debug 下这套代码会慢好几倍，测不出并发的真实表现。
#
# 【语法】if(NOT 变量 AND NOT 变量)：条件分支。
# 【语法】set(... CACHE STRING "说明" FORCE)：写缓存变量，FORCE 表示覆盖。
#         用户可以用 cmake -DCMAKE_BUILD_TYPE=Debug 覆盖。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ---- pthread ----
# C++ 的 std::thread 底层就是 pthread。
# 【知识点：为什么必须显式链接】链接器是按需拉取的：没有 -lpthread，
# std::thread 的构造函数会找不到符号。
#
# 【语法】set(THREADS_PREFER_PTHREAD_FLAG ON) 让 FindThreads 优先用
#         -pthread（它同时设置编译和链接标志），而不是只加 -lpthread。
# 【语法】find_package(Threads REQUIRED)：找不到就报错。
#         它提供一个「导入目标」Threads::Threads，后面直接链接它。
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ============================================================================
#  jobrun_core —— 把「除了 main 之外的一切」做成一个静态库
#
#  【知识点：为什么要拆出库】因为有两个可执行文件要用同一批代码：
#  正式的 jobrun，和跑自检的 selfcheck。如果两边都 add_executable 一遍源码，
#  同一份代码会被编译两次，而且修改时容易只改一边。
#  抽成库之后，「谁用谁链」——这正是 C++ 工程组织的核心手法。
#
#  【C 对照】相当于先 ar 出一个 libjobrun_core.a，再让两个程序链接它。
# ============================================================================
add_library(jobrun_core STATIC
    src/fd.cpp
    src/process.cpp
    src/job.cpp
    src/signals.cpp
    src/runner.cpp
    src/report.cpp
)

# 【知识点：PUBLIC 与 PRIVATE】include 目录用 PUBLIC：
# 链接 jobrun_core 的人也需要它才能 #include "jobrun/xxx.hpp"。
# 而警告选项只对本库自己的编译有效，用 PRIVATE 就够。
#
# 【语法】PUBLIC / PRIVATE / INTERFACE 是「属性传播」的三种方式：
#         PRIVATE  ：只影响本目标
#         PUBLIC   ：影响本目标 + 链接本目标的人
#         INTERFACE：只影响链接本目标的人（本目标自己不用）
target_include_directories(jobrun_core PUBLIC include)
target_link_libraries(jobrun_core PUBLIC Threads::Threads)

# ============================================================================
#  jobrun —— 正式的可执行文件
# ============================================================================
add_executable(jobrun src/main.cpp)
# 【语法】PRIVATE：main.cpp 自己需要 jobrun_core，但「谁链接 jobrun」
#         不需要再链接一次 jobrun_core（可执行文件是链接终点）。
target_link_libraries(jobrun PRIVATE jobrun_core)

# ============================================================================
#  selfcheck —— 仓库自带的自检，用 ctest 驱动
# ============================================================================
# 【语法】enable_testing() 打开 CTest 支持；必须在 add_test 之前调用。
enable_testing()
add_executable(selfcheck tests/selfcheck.cpp)
target_link_libraries(selfcheck PRIVATE jobrun_core)
# 【语法】add_test(NAME 名字 COMMAND 命令)：注册一个测试。
#         ctest 会执行 COMMAND，并根据退出码判断成败。
add_test(NAME selfcheck COMMAND selfcheck)

# ---- 警告 ----
# 【语法】if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")：正则匹配编译器。
#         MSVC 的警告选项不同，这里只处理 GCC/Clang。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # 【知识点：把警告当资产】-Wall -Wextra 能提前抓到绝大多数手滑。
    # 更狠的一步是加 -Werror（警告即编译失败），那对学习项目来说是好习惯，
    # 但会让「先跑起来再改」变得困难，所以这里留给你自己决定要不要开。
    #
    # 各个警告的含义：
    #   -Wall     常用警告集合
    #   -Wextra   更严格的一组（未使用参数、符号比较等）
    #   -Wpedantic 严格遵循标准的警告（例如变长数组、GNU 扩展）
    #
    # 【语法】foreach(target 值1 值2 ...) 遍历列表；${target} 展开变量。
    #         也有 foreach(target IN LISTS 变量名) 的形式。
    foreach(target jobrun_core jobrun selfcheck)
        # 【语法】PRIVATE：这些选项只用于编译本目标，不传播给链接它的人。
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()
