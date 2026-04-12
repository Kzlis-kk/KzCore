# KzCore/CMakeLists.txt

# 1. 设置 CMake 最低版本并定义项目
cmake_minimum_required(VERSION 3.16)
project(KzCore LANGUAGES CXX)

# 2. 设置全局 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 3. [关键] 将项目根目录添加到所有 Target 的 include 目录中
# 这使得在任何地方都可以使用 #include "KzAlloc/Common.h" 这样的路径
include_directories(${CMAKE_SOURCE_DIR})

# 4. 提供一个全局选项来控制是否构建所有 benchmark
option(BUILD_BENCHMARKS "Build all benchmark executables" ON)
if(BUILD_BENCHMARKS)
    message(STATUS "Benchmarks are enabled. Use -DBUILD_BENCHMARKS=OFF to disable.")
else()
    message(STATUS "Benchmarks are disabled. Use -DBUILD_BENCHMARKS=ON to enable.")
endif()

# 5. [推荐] 将通用的编译/链接选项封装成一个函数，避免重复
function(setup_kz_target_properties target_name)
    # Release 构建配置优化
    target_compile_options(${target_name} PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:Release>:-march=native>
        $<$<CONFIG:Release>:-mcx16>
        $<$<CONFIG:Release>:-fno-exceptions>
        $<$<CONFIG:Release>:-fno-rtti>
    )

    # Debug 构建配置调试信息和 Sanitizers
    set(SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls)
    target_compile_options(${target_name} PRIVATE
        $<$<CONFIG:Debug>:${SANITIZER_FLAGS}>
        $<$<CONFIG:Debug>:-O1>
        $<$<CONFIG:Debug>:-g>
    )
    target_link_options(${target_name} PRIVATE
        $<$<CONFIG:Debug>:${SANITIZER_FLAGS}>
    )

    # 启用 LTO (Link-Time Optimization)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LTO_SUPPORTED)
    if(LTO_SUPPORTED)
        message(STATUS "Link-Time Optimization (LTO) is supported and enabled.")
        set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "Link-Time Optimization (LTO) is not supported by this compiler.")
    endif()
endfunction()

# 6. 包含所有组件子目录
add_subdirectory(KzAlloc)
add_subdirectory(KzSTL)
add_subdirectory(KzLog)
add_subdirectory(KzThread)
add_subdirectory(KzNet)
add_subdirectory(KzRpc)
# ... etc