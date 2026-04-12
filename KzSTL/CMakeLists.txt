# KzCore/KzSTL/CMakeLists.txt

# 1. 收集源文件
# 使用 file(GLOB ...) 可以自动发现新文件，但新增/删除文件后需要重新运行 CMake
file(GLOB LIB_SOURCES
    "*.h"
    "*.cpp"
)

# 2. 定义库 Target，推荐 Target 名与目录名一致，更清晰
add_library(KzSTL STATIC ${LIB_SOURCES})

# 3. 调用在顶层 CMakeLists.txt 中定义的函数来设置所有通用的编译/链接属性
setup_kz_target_properties(KzSTL)

# 处理 KzSTL 特有的依赖
target_link_libraries(KzSTL PRIVATE KzAlloc)