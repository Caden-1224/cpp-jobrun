# ============================================================================
#  CMakeLists.txt —— jobrun 的构建描述
#
#  【知识点：为什么 C++ 需要一个构建系统】Rust 有 cargo，C++ 没有官方默认。
#  你的速成班大概率只教了「g++ main.cpp -o a」，那只够一个文件。
#  一旦拆成多个 .cpp，就要回答三个问题：
#    * 编译哪些文件、用哪个标准、开哪些警告   -> add_executable / target_compile_options
#    * 头文件在哪儿找                          -> target_include_directories
#    * 链接哪些库                              -> target_link_libraries
#  CMake 把这些写成一份可移植的描述，再生成 Makefile 或 Ninja。
# ============================================================================
cmake_minimum_required(VERSION 3.16)

project(jobrun
    VERSION 1.0.0
    DESCRIPTION "并发的多进程作业执行器：一份给 C++ 速成者的操作系统练习"
    LANGUAGES CXX)

# ---- 语言标准 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 【知识点：CXX_EXTENSIONS OFF】关掉编译器私有扩展，
# 用 -std=c++17 而不是 -std=gnu++17。想要可移植就别依赖方言。
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 默认构建类型 ----
# 不指定就按 Release 构建：Debug 下这套代码会慢好几倍，测不出并发的真实表现。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ---- pthread ----
# C++ 的 std::thread 底层就是 pthread。
# 【知识点：为什么必须显式链接】链接器是按需拉取的：没有 -lpthread，
# std::thread 的构造函数会找不到符号。
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ============================================================================
#  jobrun_core —— 把「除了 main 之外的一切」做成一个静态库
#
#  【知识点：为什么要拆出库】因为有两个可执行文件要用同一批代码：
#  正式的 jobrun，和跑自检的 selfcheck。如果两边都 add_executable 一遍源码，
#  同一份代码会被编译两次，而且修改时容易只改一边。
#  抽成库之后，「谁用谁链」——这正是 C++ 工程组织的核心手法。
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
target_include_directories(jobrun_core PUBLIC include)
target_link_libraries(jobrun_core PUBLIC Threads::Threads)

# ============================================================================
#  jobrun —— 正式的可执行文件
# ============================================================================
add_executable(jobrun src/main.cpp)
target_link_libraries(jobrun PRIVATE jobrun_core)

# ============================================================================
#  selfcheck —— 仓库自带的自检，用 ctest 驱动
# ============================================================================
enable_testing()
add_executable(selfcheck tests/selfcheck.cpp)
target_link_libraries(selfcheck PRIVATE jobrun_core)
add_test(NAME selfcheck COMMAND selfcheck)

# ---- 警告 ----
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # 【知识点：把警告当资产】-Wall -Wextra 能提前抓到绝大多数手滑。
    # 更狠的一步是加 -Werror（警告即编译失败），那对学习项目来说是好习惯，
    # 但会让「先跑起来再改」变得困难，所以这里留给你自己决定要不要开。
    foreach(target jobrun_core jobrun selfcheck)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()
