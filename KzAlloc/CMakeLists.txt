# KzCore/KzAlloc/CMakeLists.txt

# 1. 收集源文件
# 使用 file(GLOB ...) 可以自动发现新文件，但新增/删除文件后需要重新运行 CMake
file(GLOB LIB_SOURCES
    "*.h"
    "*.cpp"
)
# 从列表中移除 Benchmark.cpp，因为它不属于库
list(FILTER LIB_SOURCES EXCLUDE REGEX "Benchmark.cpp$")


# 2. 定义库 Target，推荐 Target 名与目录名一致，更清晰
add_library(KzAlloc STATIC ${LIB_SOURCES})

# 3. 调用在顶层 CMakeLists.txt 中定义的函数来设置所有通用的编译/链接属性
setup_kz_target_properties(KzAlloc)

# 4. 处理 KzAlloc 特有的依赖
find_package(Threads REQUIRED)
# 注意：硬编码路径是不好的实践，但暂时保留。更好的方法是使用 find_library
target_link_libraries(KzAlloc PRIVATE 
    Threads::Threads
    /usr/lib64/libatomic.so.1
)

# 5. 如果全局开关打开，则构建 benchmark
if(BUILD_BENCHMARKS)
    # 定义 benchmark 可执行文件
    add_executable(KzAlloc_benchmark Benchmark.cpp)

    # 为 benchmark 设置通用属性
    setup_kz_target_properties(KzAlloc_benchmark)

    # benchmark 需要链接到它所测试的库 KzAlloc
    target_link_libraries(KzAlloc_benchmark PRIVATE KzAlloc)
endif()