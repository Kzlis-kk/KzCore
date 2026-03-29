cmake_minimum_required(VERSION 3.16)
project(KzAlloc LANGUAGES CXX)

# 使用 C++20 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# 定义核心库的源文件
set(KZALLOC_LIB_SOURCES
    KzAlloc/CentralCache.cpp
    KzAlloc/PageCache.cpp
    KzAlloc/ThreadCache.cpp
)

# 定义 Benchmark 可执行文件的源文件
set(BENCHMARK_SOURCES
    KzAlloc/Benchmark.cpp
)


# 将内存池的核心逻辑编译为一个静态库 (Static Library)
# 这使得它可以被轻松地链接到其他项目中
add_library(kzalloc_lib STATIC ${KZALLOC_LIB_SOURCES})

# 创建 Benchmark 可执行文件
add_executable(benchmark ${BENCHMARK_SOURCES})


# 将项目根目录设为 PUBLIC include 目录
# 这样在代码中就可以使用 #include "KzAlloc/Common.h" 的形式
# PUBLIC 意味着任何链接到 kzalloc_lib 的目标（如此处的 benchmark）都会自动继承这个设置
target_include_directories(kzalloc_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})


# Release 构建配置优化
# 使用 $<CONFIG:Release> 生成器表达式，这些标志只在 Release 模式下生效
target_compile_options(kzalloc_lib PRIVATE
    $<$<CONFIG:Release>:-O3>          # 最高级别优化
    $<$<CONFIG:Release>:-DNDEBUG>     # 关闭 assert
    $<$<CONFIG:Release>:-march=native> # 针对当前编译机器的 CPU 架构进行极致优化 (AVX, FMA, etc.)
    $<$<CONFIG:Release>:-mcx16>       # 启用 128-bit CAS (CMPXCHG16B) 以支持无锁 ObjectPool
    $<$<CONFIG:Release>:-fno-exceptions> # 内存池不应使用异常，关闭以减小代码体积
    $<$<CONFIG:Release>:-fno-rtti>       # 关闭 RTTI
)
# 将相同的优化选项也应用到 Benchmark
target_compile_options(benchmark PRIVATE
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-DNDEBUG>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Release>:-mcx16>
    $<$<CONFIG:Release>:-fno-exceptions>
    $<$<CONFIG:Release>:-fno-rtti>
)

# Debug 构建配置调试信息
target_compile_options(kzalloc_lib PRIVATE $<$<CONFIG:Debug>:-g>)
target_compile_options(benchmark PRIVATE $<$<CONFIG:Debug>:-g>)
# 开启 AddressSanitizer (内存越界/UAF检测) 和 UndefinedBehaviorSanitizer (未定义行为检测)
set(SANITIZER_FLAGS 
    -fsanitize=address
    -fno-omit-frame-pointer 
    -fno-optimize-sibling-calls
)

target_compile_options(kzalloc_lib PRIVATE
    $<$<CONFIG:Debug>:${SANITIZER_FLAGS}>
    $<$<CONFIG:Debug>:-O1> # 保留一定的优化，但不会抹除堆栈信息
    $<$<CONFIG:Debug>:-g>
)
target_link_options(kzalloc_lib PRIVATE
    $<$<CONFIG:Debug>:${SANITIZER_FLAGS}>
)

target_compile_options(benchmark PRIVATE
    $<$<CONFIG:Debug>:${SANITIZER_FLAGS}>
    $<$<CONFIG:Debug>:-O1>
    $<$<CONFIG:Debug>:-g>
)
target_link_options(benchmark PRIVATE
    $<$<CONFIG:Debug>:${SANITIZER_FLAGS}>
)


# LTO 允许链接器在链接阶段进行跨编译单元的全局优化
include(CheckIPOSupported)
check_ipo_supported(RESULT LTO_SUPPORTED)
if(LTO_SUPPORTED)
    message(STATUS "Link-Time Optimization (LTO) is supported and enabled.")
    set_property(TARGET kzalloc_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET benchmark   PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(WARNING "Link-Time Optimization (LTO) is not supported by this compiler.")
endif()


# 将 benchmark 链接到我们的内存池库
# PRIVATE 表示链接关系是私有的，不会向外传递
target_link_libraries(benchmark PRIVATE kzalloc_lib)

target_link_directories(kzalloc_lib PRIVATE /usr/lib/gcc/x86_64-redhat-linux/8/libatomic.so)

# 在 Linux/macOS 下，需要链接 pthread 库以支持 std::thread
# find_package 是现代 CMake 的标准做法
find_package(Threads REQUIRED)
target_link_libraries(kzalloc_lib PRIVATE 
    Threads::Threads 
    /usr/lib64/libatomic.so.1
)