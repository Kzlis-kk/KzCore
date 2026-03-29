#pragma once

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <array>
// 平台宏判断
#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
    #include <sys/sysinfo.h>
    #include <sys/syscall.h>
#endif

namespace KzAlloc {
// 页大小配置: 8KB
static constexpr size_t PAGE_SHIFT = 13;
static constexpr size_t PAGE_SIZE = 1 << PAGE_SHIFT;
static constexpr size_t PAGE_ROUND_UP_NUM = PAGE_SIZE - 1;
static constexpr size_t PAGE_ROUND_UP_NUM_NEGATE = ~PAGE_ROUND_UP_NUM;

// 桶的总数量 (16+56+56+112+24 = 264)
// [1, 128B]: 8B 对齐 (8, 16, ... 128)
// [129B, 1024B]: 16B 对齐
// [1KB, 8KB]: 128B 对齐
// [8KB, 64KB]: 512B 对齐
// [64KB, 256KB]: 8KB 对齐
static constexpr int MAX_NFREELISTS = 264;
static constexpr size_t MAX_BYTES = 256 * 1024;
// =========================================================================
// SizeUtils
// 管理内存对齐和桶映射的静态工具集
// =========================================================================
namespace SizeUtils {

namespace detail {
    // 辅助：超大内存对齐
    inline size_t RoundUpToPage(size_t size) noexcept {
        return (size + PAGE_ROUND_UP_NUM) & PAGE_ROUND_UP_NUM_NEGATE;
    }

    // 根据当前桶的大小，决定下一个桶应该大多少
    // 只有在 Init 时调用，稍微多几个 if-else 没关系
    consteval size_t CalculateNextBlockSize(size_t current_size) noexcept {
        // [1, 128] -> 8B 对齐
        // 这里判断的是 current_size，也就是“上一个桶的大小”
        if (current_size < 128) {
            return current_size + 8;
        }
        // [129, 1024] -> 16B 对齐
        else if (current_size < 1024) {
            return current_size + 16;
        }
        // [1K, 8K] -> 128B 对齐
        else if (current_size < (1024 << 3)) {
            return current_size + 128;
        }
        // [8K, 64K] -> 512B 对齐
        else if (current_size < (1024 << 6)) {
            return current_size + 512;
        }
        // [64K, 256K] -> 8KB 对齐
        else {
            return current_size + (1024 << 3);
        }
    }

    consteval auto GenerateClassToSizeTable() noexcept {
        std::array<size_t, MAX_NFREELISTS> table{};
        size_t block_size = 8;
        int index = 0;
        table[0] = 8;
        for (size_t i = 1; i <= MAX_BYTES; ++i) {
            if (i > block_size) {
                index++;
                block_size = CalculateNextBlockSize(block_size);
                if (index < MAX_NFREELISTS) {
                    table[index] = block_size;
                }
            }
        }
        return table;
    }

    consteval auto GenerateFastIndexTable() noexcept {
        // 最小对齐为 8 字节，所以前三位可以直接忽略
        // 32768 / 8 = 4096 个元素
        std::array<uint8_t, 4096> table{}; 
        
        // 遍历 1 到 32768 字节，计算并填充 Index
        for (size_t i = 1; i <= 32768; ++i) {
            // 复用你原来的 Index 逻辑来生成表
            size_t idx;
            if (i <= 128) {
                idx = ((i + 7) >> 3) - 1;
            } else if (i <= 1024) {
                idx = 15 + ((i - 128 + 15) >> 4);
            } else if (i <= 8 * 1024) {
                idx = 71 + ((i - 1024 + 127) >> 7);
            } else {
                idx = 127 + ((i - 8192 + 511) >> 9);
            }
            
            // 映射到 0~4095 的数组下标
            // 例如: i=1~8 -> 下标0; i=9~16 -> 下标1
            table[(i - 1) >> 3] = static_cast<uint8_t>(idx);
        }
        return table;
    }
} // namespace detail

// 编译期生成的静态常量表，存储在 .rodata 段，运行时毫无开销
inline constexpr auto _class_to_size = detail::GenerateClassToSizeTable();
inline constexpr auto _fast_size_to_class = detail::GenerateFastIndexTable();
    
// 输入 size，返回对应的桶编号 (0 ~ 263)
inline int Index(size_t size) noexcept {
    assert(size <= MAX_BYTES);
    if (size == 0) [[unlikely]] return 0;

    // 快速路径：32KB 以下直接查表，主要是消除分支，优化流水线
    if (size <= 32768) [[likely]] {
        return _fast_size_to_class[(size - 1) >> 3];
    }

    // 慢速路径：32KB ~ 256KB (走计算逻辑)
    if (size <= 64 * 1024) {
        return 127 + ((size - 8192 + 511) >> 9);
    } else {
        return 239 + ((size - 65536 + 8191) >> 13);
    }
}

// 输入 size，返回对齐后的大小 (例如输入 13 返回 16)
inline size_t RoundUp(size_t size) noexcept {
    if (size > MAX_BYTES)  return detail::RoundUpToPage(size);
    return _class_to_size[Index(size)];
}

// 获取某个桶里的块有多大
inline size_t Size(size_t index) noexcept {
    assert(index < MAX_NFREELISTS);
    return _class_to_size[index];
}

// 一次性获取数量，供 ThreadCache 使用
inline size_t NumMoveSize(size_t index) noexcept {
    size_t num = MAX_BYTES / _class_to_size[index];
    if (num < 2) return 2;
    if (num > 32768) return 32768;
    return num;
}

};

} // namespace KzAlloc