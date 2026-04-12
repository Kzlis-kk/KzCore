# KzCore/KzRpc/CMakeLists.txt

# 1. 收集源文件
# 使用 file(GLOB ...) 可以自动发现新文件，但新增/删除文件后需要重新运行 CMake
file(GLOB LIB_SOURCES
    "*.h"
    "*.cpp"
)
# 从列表中移除 Benchmark.cpp，因为它不属于库
list(FILTER LIB_SOURCES EXCLUDE REGEX "Benchmark.cpp$")


# 2. 定义库 Target，推荐 Target 名与目录名一致，更清晰
add_library(KzRpc STATIC ${LIB_SOURCES})

# 3. 调用在顶层 CMakeLists.txt 中定义的函数来设置所有通用的编译/链接属性
setup_kz_target_properties(KzRpc)

# 4. 处理 KzRpc 特有的依赖
target_link_libraries(KzRpc PRIVATE KzAlloc)
target_link_libraries(KzRpc PRIVATE KzSTL)
target_link_libraries(KzRpc PRIVATE KzLog)
target_link_libraries(KzRpc PRIVATE KzThread)
target_link_libraries(KzRpc PRIVATE KzNet)

# 5. 如果全局开关打开，则构建 benchmark
if(BUILD_BENCHMARKS)
    # 定义 benchmark 可执行文件
    add_executable(KzRpc_benchmark Benchmark.cpp)

    # 为 benchmark 设置通用属性
    setup_kz_target_properties(KzRpc_benchmark)

    # benchmark 需要链接到它所测试的库 KzLog
    target_link_libraries(KzRpc_benchmark PRIVATE KzNet)
endif()