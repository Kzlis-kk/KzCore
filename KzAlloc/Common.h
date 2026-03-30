#pragma once

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <mutex>
#include <cassert>
#include <new>
#include <cstdio>
#include "SizeUtils.h"

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

#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    constexpr size_t kCacheLineSize = 64;
#endif

// 辅助函数
inline void*& NextObj(void* obj) noexcept {
    return *reinterpret_cast<void**>(obj);
}

inline void handleOOM() noexcept {
    printf("FATAL ERROR:: OOM");
    abort();
}

#ifdef _WIN64
    typedef uint64_t PAGE_ID;
#elif _WIN32
    typedef uint32_t PAGE_ID;
#else
    typedef uint64_t PAGE_ID;
#endif

// 获取系统物理内存总大小 (字节)
inline size_t GetSystemPhysicalMemory() noexcept {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return static_cast<size_t>(status.ullTotalPhys);
#else
    // Linux 获取物理内存
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) [[likely]] {
        return static_cast<size_t>(pages) * page_size;
    }
#endif
    // 兜底：如果获取失败，默认假设 8GB (生产环境应处理 error)
    return 8ULL * 1024 * 1024 * 1024;
}

// 2MB 阈值，超过这个值尝试申请大页 (Linux 默认大页通常是 2MB)
inline constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;
inline constexpr size_t HUGE_PAGE_MASK = ~(HUGE_PAGE_SIZE - 1);
// 向系统申请 kpage 页的内存
// 可优化点：可以实现一个 SystemHeap,
// 每次向 OS 批发一大块（比如 1GB，使用 mmap，不需要切头去尾，因为 1GB 必然是 8KB 对齐的）,
// 然后在用户态自己切分
[[nodiscard]] inline void* SystemAlloc(size_t kpage) noexcept {
    size_t size = kpage << PAGE_SHIFT;
    
#ifdef _WIN32
    // Windows 的 VirtualAlloc 默认分配粒度是 64KB
    return VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    // Hot Path: 纯大页分配 (2MB 的整数倍)
    if ((size & ~HUGE_PAGE_MASK) == 0) [[likely]] {
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
        
        // OS 保证大页 2MB 对齐
        if (ptr != MAP_FAILED) [[likely]] {
            return ptr;
        }
        // 如果大页失败 (系统未开启或碎片过多)，则 Fallback 到下面的通用逻辑
    }

    // Cold Path: 大页 + 小页混合分配 或 纯小页分配
    // 虚拟地址占位
    size_t reserve_size = size + HUGE_PAGE_SIZE;
    void* reserve_ptr = mmap(nullptr, reserve_size, PROT_NONE, 
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserve_ptr == MAP_FAILED) [[unlikely]] return nullptr;

    // 计算 2MB 对齐的起始地址
    uintptr_t raw_addr = reinterpret_cast<uintptr_t>(reserve_ptr);
    uintptr_t aligned_addr = (raw_addr + HUGE_PAGE_SIZE - 1) & HUGE_PAGE_MASK;

    // 切除头尾多余的虚拟地址
    size_t prefix_len = aligned_addr - raw_addr;
    if (prefix_len > 0) munmap(reserve_ptr, prefix_len);
    
    size_t suffix_len = reserve_size - size - prefix_len;
    if (suffix_len > 0) munmap(reinterpret_cast<void*>(aligned_addr + size), suffix_len);

    void* final_ptr = reinterpret_cast<void*>(aligned_addr);

    // 大页 + 小页混合物理映射 (MAP_FIXED 覆盖)
    size_t huge_part = size & HUGE_PAGE_MASK;
    size_t small_part = size - huge_part;

    if (huge_part > 0) [[likely]] {
        void* huge_ptr = mmap(final_ptr, huge_part, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE | MAP_FIXED, -1, 0);
        if (huge_ptr == MAP_FAILED) [[unlikely]] {
            // Fallback: 如果大页失败，用普通页覆盖
            mmap(final_ptr, huge_part, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        }
    }
    assert(small_part > 0);
    void* small_ptr_start = reinterpret_cast<void*>(aligned_addr + huge_part);
    mmap(small_ptr_start, small_part, PROT_READ | PROT_WRITE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    
    return final_ptr;
#endif
}

// 释放内存归还给系统
inline void SystemFree(void* ptr, size_t kpage) noexcept {
    if (ptr == nullptr) return;

#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    // Linux munmap
    size_t size = kpage << PAGE_SHIFT;
           
    munmap(ptr, size);
#endif
}


} // namespace KzAlloc