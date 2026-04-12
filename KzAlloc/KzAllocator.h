#pragma once

#include "ConcurrentAlloc.h"
#include <limits>
#include <utility>
#include <memory>

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

    // 明确告诉 STL，不要在拷贝/移动赋值时传播分配器
    // 这会强制 std::vector 在赋值时采取最保守、最安全的元素逐个拷贝/销毁策略
    // using propagate_on_container_copy_assignment = std::false_type;
    //using propagate_on_container_move_assignment = std::false_type;
    // using propagate_on_container_swap = std::false_type;

    // Rebind 结构
    // 告诉 STL 如何把 KzAllocator<T> 变成 KzAllocator<U>
    template <class U>
    struct rebind {
        using other = KzAllocator<U>;
    };

    KzAllocator() noexcept = default;

    template <class U>
    constexpr KzAllocator(const KzAllocator<U>&) noexcept {};

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
        void* ptr = nullptr;
        size_t total_size = n * sizeof(T);

        // 编译期分支：如果类型 T 的对齐要求大于默认对齐
        if constexpr (alignof(T) > kMinAlignment) {
            // 调用对齐分配接口
            ptr = KzAlloc::malloc_aligned(total_size, alignof(T));
        } else {
            // 普通分配
            ptr = KzAlloc::malloc(total_size);
        }
        
        if (ptr) [[likely]] {
            return static_cast<T*>(ptr);
        }
        
        handleOOM();
    }

    // 释放内存
    void deallocate(T* p, size_t n) noexcept {
        if constexpr (alignof(T) > kMinAlignment) {
            KzAlloc::free_aligned(p, n * sizeof(T), alignof(T));
        } else {
            KzAlloc::free(p, n * sizeof(T));
        }
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

// 针对 std::shared_ptr<U> 的模板偏特化
// 解决编译器在 -O3 下对内联 free 和 shared_ptr 控制块析构的激进指令重排问题
template <class U>
class KzAllocator<std::shared_ptr<U>> {
public:
    // STL 标准类型定义
    using value_type = std::shared_ptr<U>;
    using pointer = std::shared_ptr<U>*;
    using const_pointer = const std::shared_ptr<U>*;
    using reference = std::shared_ptr<U>&;
    using const_reference = const std::shared_ptr<U>&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    // Rebind 结构
    template <class V>
    struct rebind {
        using other = KzAllocator<V>;
    };

    // 强制 propagate 为 false_type
    // 这会引导 STL 容器（如 std::vector）在移动赋值时，采取更保守的策略
    // 从而避免编译器将 KzAlloc::free 的内联逻辑与 shared_ptr 控制块的析构进行有害的指令重排
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;

    // 构造函数
    KzAllocator() noexcept = default;
    
    template <class V>
    KzAllocator(const KzAllocator<V>&) noexcept {};

    ~KzAllocator() = default;

    // 申请内存
    std::shared_ptr<U>* allocate(size_t n) {
        if (n > std::numeric_limits<size_t>::max() / sizeof(std::shared_ptr<U>)) [[unlikely]] {
            handleOOM();
        }
        if (void* ptr = KzAlloc::malloc(n * sizeof(std::shared_ptr<U>))) {
            return static_cast<std::shared_ptr<U>*>(ptr);
        }
        handleOOM();
    }

    // 释放内存
    void deallocate(std::shared_ptr<U>* p, size_t n) noexcept {
        KzAlloc::free(p, n * sizeof(std::shared_ptr<U>));
    }

    // 对象构造
    template <class V, class... Args>
    void construct(V* p, Args&&... args) {
        std::construct_at(p, std::forward<Args>(args)...);
    }

    // 对象析构
    template <class V>
    void destroy(V* p) {
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