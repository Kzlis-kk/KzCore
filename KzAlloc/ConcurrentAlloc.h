#pragma once

#include "Common.h"
#include "ThreadCache.h"
#include "PageCache.h"
#include "ObjectPool.h"

namespace KzAlloc {

namespace detail {
    // 将 ThreadCache 的池子定义在 detail 命名空间中
    // 确保全局唯一的单例
    inline ObjectPool<ThreadCache>& GetThreadCachePool() noexcept {
        // 使用函数局部静态变量的线程安全初始化特性，保证只创建一个实例
        static ObjectPool<ThreadCache> tcPool;
        return tcPool;
    }
} // namespace detail


// ThreadCache 创建器
inline void* CreateThreadCache() noexcept {
    return detail::GetThreadCachePool().New();
}

// ThreadCache 销毁器
inline void DestroyThreadCache(void* ptr) noexcept {
    detail::GetThreadCachePool().Delete(static_cast<ThreadCache*>(ptr));
}

// 裸指针，POD 类型
inline thread_local ThreadCache* tls_cache = nullptr;
// 用于生命周期管理的守卫
class ThreadCacheManager {
public:
    ~ThreadCacheManager() {
        if (tls_cache) [[likely]] {
            DestroyThreadCache(tls_cache);
            tls_cache = nullptr;
        }
    }
};
inline thread_local ThreadCacheManager tls_manager_guard;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#elif defined(_MSC_VER)
__forceinline
#endif
inline ThreadCache* GetThreadCache() noexcept {
    if (tls_cache == nullptr) [[unlikely]] {
        tls_cache = static_cast<ThreadCache*>(CreateThreadCache());
        (void)tls_manager_guard; // 触碰守卫
    }
    return tls_cache;
}

// ==========================================================
// 核心申请接口
// ==========================================================
inline void* malloc(size_t size) noexcept {
    // 处理超大内存 (> 256KB)
    // PageCache 的对齐单位是页 (8KB)
    if (size > MAX_BYTES) [[unlikely]] {
        // 向上对齐到页大小
        size_t alignSize = SizeUtils::RoundUp(size);
        size_t kPages = alignSize >> PAGE_SHIFT;

        // 向 PageHeap 申请
        Span* span = PageHeap::GetInstance()->NewSpan(kPages);
        if (!span) [[unlikely]] return nullptr;
        span->_objSize = alignSize;
        span->_isUse = true;

        // 计算返回地址
        void* ptr = reinterpret_cast<void*>(span->_pageId << PAGE_SHIFT);
        return ptr;
    }

     if (tls_cache == nullptr) [[unlikely]] {
        tls_cache = static_cast<ThreadCache*>(CreateThreadCache());
        static_cast<void>(tls_manager_guard); // 触碰一下 guard，确保编译器为其注册析构函数
    }
    return GetThreadCache()->Allocate(size);
}

// ==========================================================
// 核心释放接口
// ==========================================================
inline void free(void* ptr) noexcept {
    if (ptr == nullptr) return;

    // 通过地址反查 Span
    // 我们的 PageMap 记录了每个页对应的 Span 指针
    PAGE_ID id = reinterpret_cast<PAGE_ID>(ptr) >> PAGE_SHIFT;
    Span* span = PageMap::GetInstance()->get(id);

    // 这里的 span 不应该为空，除非用户释放了野指针
    assert(span != nullptr);
    size_t size = span->_objSize;
    // 判断是大内存还是小内存
    if (size > MAX_BYTES) [[unlikely]] {
        // 大内存：直接还给 PageHeap
        PageHeap::GetInstance()->ReleaseSpan(span);
    } else {
        // 小内存：还给 ThreadCache
        GetThreadCache()->Deallocate(ptr, size);
    }
}

inline void free(void* ptr, size_t size) noexcept {
    if (size > MAX_BYTES) [[unlikely]]{
            PAGE_ID id = reinterpret_cast<PAGE_ID>(ptr) >> PAGE_SHIFT;
            Span* span = PageMap::GetInstance()->get(id);
            PageHeap::GetInstance()->ReleaseSpan(span);
            return;
        }
    // 小对象直接走 TLS，跳过 PageMap 查找
    GetThreadCache()->Deallocate(ptr, size);
}


// 辅助函数：获取指针指向的内存块的实际大小
// 用于 realloc 时计算拷贝数据量
inline size_t GetAllocatedSize(void* ptr) noexcept {
    // 查 PageMap 找到 Span
    PAGE_ID id = (PAGE_ID)ptr >> PAGE_SHIFT;
    Span* span = PageMap::GetInstance()->get(id);
    
    // 如果是野指针或未托管内存，span 可能为空 (需谨慎)
    if (span == nullptr) return 0;
    
    // 返回 Span 记录的对象大小
    return span->_objSize;
}

// ==========================================================
// 优化版 Realloc (Sized Realloc)
// 场景：STL 容器扩容，或者用户知道原始大小
// 优势：完全跳过 PageMap 查找，O(1) 完成
// ==========================================================
inline void* realloc(void* ptr, size_t old_size, size_t new_size) noexcept {
    // 处理特殊情况
    if (ptr == nullptr) [[unlikely]] {
        return KzAlloc::malloc(new_size);
    }
    if (new_size == 0) [[unlikely]] {
        KzAlloc::free(ptr, old_size);
        return nullptr;
    }

    // 计算对齐后的大小
    // 注意：这里即使 old_size 是未对齐的 (如 13)，RoundUp 后也会变成 16，与 ThreadCache 逻辑一致
    size_t old_aligned = SizeUtils::RoundUp(old_size);
    size_t new_aligned = SizeUtils::RoundUp(new_size);

    // 原地复用策略
    // 情况 A: 规格相同 (例如从 17 扩容到 25，都属于 32 字节桶) -> 直接返回
    if (new_aligned == old_aligned) {
        return ptr;
    }
    
    // 情况 B: 缩容 (Shrink) -> 懒惰策略
    // 如果新大小比旧大小小，为了避免频繁抖动，我们通常选择不搬迁
    // (除非差异巨大，但在通用内存池中，保留原块通常是更优解)
    if (new_aligned < old_aligned) [[unlikely]] {
        return ptr;
    }

    // 异地扩容
    void* new_ptr = KzAlloc::malloc(new_size);
    if (new_ptr != nullptr) {
        // 核心优化：只拷贝用户声称的有效数据长度
        // 这里 old_size 可能小于 old_aligned (例如用户实际只用了 13 字节)
        // 拷贝 13 字节即可，虽然拷贝 16 字节也安全，但精确拷贝指令更少
        std::memcpy(new_ptr, ptr, old_size);
        
        // 释放旧内存 (调用 Sized Free，不查 PageMap)
        KzAlloc::free(ptr, old_size);
    }

    return new_ptr;
}

// ==========================================================
// 标准版 Realloc
// 场景：兼容标准 C 接口，或者不知道旧大小时使用
// 代价：需要查 PageMap，多一次内存访问
// ==========================================================
inline void* realloc(void* ptr, size_t new_size) noexcept {
    if (ptr == nullptr) [[unlikely]] return KzAlloc::malloc(new_size);
    if (new_size == 0) [[unlikely]] {
        KzAlloc::free(ptr);
        return nullptr;
    }

    // 复用优化版逻辑
    // 这里传入 old_aligned_size 作为 old_size
    // memcpy 会拷贝整个对齐块 (包括 padding)，这是安全的
    return KzAlloc::realloc(ptr, GetAllocatedSize(ptr), new_size);
}

// 辅助函数：计算对齐分配的实际底层请求大小
inline size_t GetAlignedRequestSize(size_t size, size_t alignment) noexcept {
    // 必须与 malloc_aligned 中的计算逻辑保持严格一致
    return size + alignment + sizeof(void*);
}

// ==========================================================
// 对齐内存分配接口 (Aligned Allocation)
// 场景：SIMD 优化、Cache Line 对齐、硬件设备缓冲区
// ==========================================================
inline void* malloc_aligned(size_t size, size_t alignment) noexcept {
    // 校验对齐参数
    // 对齐必须是 2 的幂 (e.g., 8, 16, 64, 4096)
    assert(alignment != 0 && !(alignment & (alignment - 1)));
    // 对齐不能小于指针大小 (为了存放 Header)
    if (alignment < sizeof(void*)) {
        alignment = sizeof(void*);
    }

    // 计算总申请大小
    // 我们需要额外的空间来调整地址，以及存放原始指针(Header)
    // Worst case padding: alignment - 1
    // Header size: sizeof(void*)
    size_t request_size = GetAlignedRequestSize(size, alignment);

    // 调用底层分配
    void* raw_ptr = KzAlloc::malloc(request_size);
    if (raw_ptr == nullptr) [[unlikely]] return nullptr;

    // 计算对齐地址
    uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw_ptr);
    // 先留出 Header 的空间，然后向上对齐
    uintptr_t aligned_addr = (raw_addr + sizeof(void*) + (alignment - 1)) & ~(alignment - 1);

    // 记录原始指针 (Header)
    // 在对齐地址的前一个指针位置存放 raw_ptr
    (reinterpret_cast<void**>(aligned_addr))[-1] = raw_ptr;

    return reinterpret_cast<void*>(aligned_addr);
}

inline void free_aligned(void* ptr) noexcept {
    if (ptr == nullptr) [[unlikely]] return;

    // 取出 Header 中的原始指针
    void* raw_ptr = (reinterpret_cast<void**>(ptr))[-1];

    // 释放原始内存块
    // 注意：这里必须调用 KzAlloc::free，因为它会去查 PageMap
    KzAlloc::free(raw_ptr);
}

// ==========================================================
// 优化版 Aligned Free (Sized Deallocation)
// 场景：用户明确知道对象的原始大小和对齐参数
// 优势：完全跳过 PageMap 查找，性能极高
// ==========================================================
inline void free_aligned(void* ptr, size_t size, size_t alignment) noexcept {
    if (ptr == nullptr) [[unlikely]] return;

    // 必须重新计算当初 malloc 时请求的总大小
    // 公式必须与 malloc_aligned 保持严格一致
    // 这里的 alignment 必须是当初 malloc 传入的同一个值
    size_t request_size = GetAlignedRequestSize(size, alignment);

    // 获取原始指针 (Header)
    void* raw_ptr = (reinterpret_cast<void**>(ptr))[-1];

    // 调用底层的 Sized Free
    // 这会直接计算 SizeClass 并归还给 ThreadCache，不查 PageMap
    KzAlloc::free(raw_ptr, request_size);
}

// ==========================================================
// 智能版 Aligned Realloc (支持原地复用)
// 场景：不知道旧大小时使用
// ==========================================================
inline void* realloc_aligned(void* ptr, size_t new_size, size_t alignment) noexcept {
    // 1. 特殊情况处理
    if (ptr == nullptr) [[unlikely]] return KzAlloc::malloc_aligned(new_size, alignment);
    if (new_size == 0) [[unlikely]] {
        KzAlloc::free_aligned(ptr);
        return nullptr;
    }

    // 获取元数据
    // 读取 Header 里的原始指针
    void* raw_ptr = (reinterpret_cast<void**>(ptr))[-1];
    
    // 查 PageMap 获取该块的物理总容量 (Span 的 objSize)
    // 注意：这是底层分配的对齐后的大小 (例如 32, 64...)
    size_t total_block_size = GetAllocatedSize(raw_ptr);

    // 计算当前指针对齐后的偏移量
    size_t offset = (uintptr_t)ptr - (uintptr_t)raw_ptr;

    // 计算当前位置实际可用的剩余容量
    size_t available_capacity = total_block_size - offset;

    // 尝试原地复用 (In-Place Reuse)
    // 情况 A: 缩容 (Shrink) -> 总是复用
    // 情况 B: 扩容 (Grow) 但仍在物理块范围内 -> 复用
    if (new_size <= available_capacity) {
        // 不需要做任何事，直接返回原指针
        // 内存池不管理逻辑大小，只管理物理块，所以这里不需要通知内存池
        return ptr;
    }

    // 无法复用，必须搬迁 (Fallback)
    void* new_ptr = KzAlloc::malloc_aligned(new_size, alignment);
    if (new_ptr != nullptr) {
        // 拷贝数据
        // 有效数据长度是：旧的可用容量 vs 新大小 的较小值
        // 注意：这里我们假设旧的有效数据填满了 available_capacity (或者更少)
        // 为了安全，我们只能拷贝 available_capacity 长度，
        // 但实际上用户可能只用了更少。由于不知道 old_size，只能按最大可能拷贝。
        // *性能注记*：如果用户存的数据很少，这里可能会多拷贝一些垃圾数据，但这是安全的。
        size_t copy_len = (available_capacity < new_size) ? available_capacity : new_size;
        std::memcpy(new_ptr, ptr, copy_len);
        
        // 释放旧块
        KzAlloc::free_aligned(ptr);
    }

    return new_ptr;
}

// ==========================================================
// 极速版 Aligned Realloc
// 场景：已知旧大小，追求极致性能
// ==========================================================
inline void* realloc_aligned(void* ptr, size_t old_size, size_t new_size, size_t alignment) noexcept {
    if (ptr == nullptr) [[unlikely]] return KzAlloc::malloc_aligned(new_size, alignment);
    if (new_size == 0) [[unlikely]] {
        KzAlloc::free_aligned(ptr, old_size, alignment);
        return nullptr;
    }

    // 计算底层 Size Class 需求
    // 我们不关心具体的 offset，我们只关心：
    // "为了满足 old_size + alignment，底层分配了多大的块？"
    // "为了满足 new_size + alignment，底层需要分配多大的块？"
    size_t old_req = GetAlignedRequestSize(old_size, alignment);
    size_t new_req = GetAlignedRequestSize(new_size, alignment);

    // 获取对齐后的 Size Class 大小 (例如 17->32, 24->32)
    size_t old_class_size = SizeUtils::RoundUp(old_req);
    size_t new_class_size = SizeUtils::RoundUp(new_req);

    // 尝试原地复用
    // 如果新的 Size Class 不大于旧的 Size Class，说明物理空间绝对够用！
    // 即使 new_size > old_size，只要它们落在同一个 Size Class 区间内，就可以复用。
    if (new_class_size <= old_class_size) {
        return ptr;
    }

    // 异地搬迁
    void* new_ptr = KzAlloc::malloc_aligned(new_size, alignment);
    if (new_ptr != nullptr) {
        // 精确拷贝：因为知道 old_size，所以只拷贝有效数据
        size_t copy_len = (old_size < new_size) ? old_size : new_size;
        std::memcpy(new_ptr, ptr, copy_len);
        
        // 释放旧块 (使用 Sized Free 优化)
        KzAlloc::free_aligned(ptr, old_size, alignment);
    }

    return new_ptr;
}

} // namespace KzAlloc