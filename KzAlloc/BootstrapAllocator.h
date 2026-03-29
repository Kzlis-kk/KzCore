#pragma once
#include "Common.h"
#include "ObjectPool.h"
#include <limits>
#include <type_traits>

namespace KzAlloc {

// 这是一个专门给内存池内部容器（如 PageCache 中的 std::map）使用的分配器
// 它保证不调用 global operator new，也不经过 ThreadCache，从而避免递归死锁
template <typename T>
class BootstrapAllocator {

public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    // 标准 Rebind 结构，兼容 std::list / std::map 等节点型容器
    template <typename U>
    struct rebind {
        using other = BootstrapAllocator<U>;
    };

    explicit BootstrapAllocator() noexcept = default;
    
    template <typename U>
    BootstrapAllocator(const BootstrapAllocator<U>&) noexcept {}

    ~BootstrapAllocator() = default;

    // 核心分配接口
    // 其实只适用于节点型容器，也就是能走对象池的，因为批量时申请内存至少也是一页，一般一页都是远远溢出的，会浪费大量内存
    T* allocate(size_t n) {
        // 场景 1: 单个对象分配 (std::map/std::list 节点的典型场景)
        // 走 ObjectPool，速度最快，且完全自举
        if (n == 1) [[likely]] {
            return GetPool().AllocateMemory();
        }

        // 场景 2: 数组分配 (std::vector 扩容场景)
        // 走 SystemAlloc 直接向 OS 批发，避免 ObjectPool 处理不了大块连续内存（ObjectPool是单块链表式，块与块之间不保证连续）
        if (n > std::numeric_limits<size_t>::max() / sizeof(T)) [[unlikely]] {
            handleOOM();
        }
        
        size_t bytes = n * sizeof(T);
        // 计算需要的页数 (向上取整)
        size_t kpages = (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
        
        void* ptr = SystemAlloc(kpages);
        return static_cast<T*>(ptr);
    }

    // 核心释放接口
    void deallocate(T* p, size_t n) {
        if (p == nullptr) return;

        // 场景 1: 单个对象回收
        if (n == 1) {
            GetPool().FreeMemory(p);
            return;
        }

        // 场景 2: 数组回收
        size_t bytes = n * sizeof(T);
        size_t kpages = (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
        SystemFree(p, kpages);
    }

    // 对象构造 (Placement New)
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        // new(p) U(std::forward<Args>(args)...);
        std::construct_at(p, std::forward<Args>(args)...); // C++20
    }

    // 对象析构
    template <typename U>
    void destroy(U* p) {
        // p->~U();
        std::destroy_at(p); // C++20
    }

private:
    // 静态单例 ObjectPool
    static ObjectPool<T>& GetPool() {
        static ObjectPool<T> pool;
        return pool;
    }
};

// 所有的 BootstrapAllocator 都是无状态的，因此被视为相等
template <typename T, typename U>
bool operator==(const BootstrapAllocator<T>&, const BootstrapAllocator<U>&) { return true; }

template <typename T, typename U>
bool operator!=(const BootstrapAllocator<T>&, const BootstrapAllocator<U>&) { return false; }

} // namespace KzAlloc