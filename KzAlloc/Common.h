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
static constexpr size_t HUGE_PAGE_THRESHOLD = 2 * 1024 * 1024;
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
    // Linux mmap
    void* ptr = MAP_FAILED;

    // 尝试大页 (2MB)
    if (size >= HUGE_PAGE_THRESHOLD) [[unlikely]] {
        ptr = mmap(0, size, PROT_READ | PROT_WRITE, 
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
        if (ptr != MAP_FAILED) [[likely]] {
            return ptr;
        }
    }
    

    if constexpr (PAGE_SIZE <= 4096) {
        // 4KB 页逻辑 (直接申请)
        ptr = mmap(0, size, PROT_READ | PROT_WRITE, 
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr != MAP_FAILED) [[likely]] {
            return ptr;
        }
    }
    else {
        // Fallback 逻辑 (4KB页 -> 手动对齐)
        // 多申请一页用于调整
        size_t allocSize = size + PAGE_SIZE;
    
        void* raw_ptr = mmap(0, allocSize, PROT_READ | PROT_WRITE, 
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                   
        if (raw_ptr == MAP_FAILED) [[unlikely]] return nullptr;
    
        // 向上对齐，必须转为 uintptr_t 才能做位运算，不能用 char* 直接 &
        uintptr_t aligned_addr = (reinterpret_cast<uintptr_t>(raw_ptr) + PAGE_ROUND_UP_NUM) & PAGE_ROUND_UP_NUM_NEGATE;
    
        // 切除头部
        size_t prefix_len = aligned_addr - reinterpret_cast<uintptr_t>(raw_ptr);
        if (prefix_len > 0) {
            munmap(raw_ptr, prefix_len);
        }
    
        // 切除尾部
        size_t suffix_len = allocSize - size - prefix_len;
        if (suffix_len > 0) {
            munmap(reinterpret_cast<void*>(aligned_addr + size), suffix_len);
        }
    
        return reinterpret_cast<void*>(aligned_addr);
    }
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