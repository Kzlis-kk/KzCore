#pragma once

#include "ConcurrentAlloc.h"
#include <limits>
#include <utility>

namespace KzAlloc {

template <class T>
class KzAllocator {
public:
    // STL 标准类型定义
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    // Rebind 结构
    // 告诉 STL 如何把 KzAllocator<T> 变成 KzAllocator<U>
    template <class U>
    struct rebind {
        using other = KzAllocator<U>;
    };

    KzAllocator() noexcept = default;

    ~KzAllocator() = default;

    // 申请内存 (n 个 T 对象)
    T* allocate(size_t n) {
        // 防止溢出
        if constexpr (sizeof(T) > 1) {
            if (n > std::numeric_limits<size_t>::max() / sizeof(T)) [[unlikely]] {
                handleOOM();
            }
        }
        
        // 调用高并发内存配置器
        if (void* ptr = KzAlloc::malloc(n * sizeof(T))) {
            return static_cast<T*>(ptr);
        }
        
        handleOOM();
    }

    // 释放内存
    void deallocate(T* p, size_t n) noexcept {
        KzAlloc::free(p, n * sizeof(T));
    }

    // 对象构造 (Placement New)
    template <class U, class... Args>
    void construct(U* p, Args&&... args) {
        std::construct_at(p, std::forward<Args>(args)...);
    }

    // 对象析构
    template <class U>
    void destroy(U* p) {
        std::destroy_at(p);
    }
};

// 比较运算符
// 因为无状态，所有分配器实例都是等价的
template <class T, class U>
bool operator==(const KzAllocator<T>&, const KzAllocator<U>&) {
    return true;
}

template <class T, class U>
bool operator!=(const KzAllocator<T>&, const KzAllocator<U>&) {
    return false;
}

} // namespace KzAlloc