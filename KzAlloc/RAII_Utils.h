#pragma once
#include "KzAllocator.h"
#include <memory>
#include <utility>
#include <type_traits>

namespace KzAlloc {
// 默认 Deleter
// 使用 std::allocator_traits 保证最大兼容性
template <typename Alloc, typename T>
struct DefaultDeleter {
    void operator()(Alloc& alloc, T* ptr) const noexcept {
        using Traits = std::allocator_traits<Alloc>;
        Traits::destroy(alloc, ptr);       // 调用析构函数
        Traits::deallocate(alloc, ptr, 1); // 释放内存
    }
};

// 泛型 Deleter
// 模板参数 T: 对象类型
// 模板参数 Alloc: 分配器类型
// 模板参数 Func: 释放逻辑，默认为 DefaultDeleter
template <
    typename T, 
    typename Alloc, 
    typename Func = DefaultDeleter<Alloc, T>
>
struct AllocatorDeleter {
    // 无状态成员使用空基类优化
    [[no_unique_address]] Alloc alloc_;
    [[no_unique_address]] Func func_;

    // 构造函数
    AllocatorDeleter() noexcept = default; 
    explicit AllocatorDeleter(const Alloc& alloc, Func func = Func{}) noexcept 
        : alloc_(alloc), func_(std::move(func)) {}

    // unique_ptr 销毁时调用的 operator()
    void operator()(T* ptr) noexcept {
        if (ptr) [[likely]] {
            func_(alloc_, ptr); // 将分配器和指针传给具体的释放逻辑
        }
    }
};

// allocate_unique 工厂函数，仅支持无状态内存配置器
template <
    typename T, 
    typename Alloc = KzAlloc::KzAllocator<T>,
    typename... Args
>
auto allocate_unique(Args&&... args) {
    // 不允许构造函数抛异常
    static_assert(!std::is_move_constructible_v<T> || std::is_nothrow_move_constructible_v<T>,
    "If T is move constructible, it must be nothrow move constructible.");
    static_assert(!std::is_move_assignable_v<T> || std::is_nothrow_move_assignable_v<T>,
    "If T is move assignable, it must be nothrow move assignable.");
    static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");

    // 在函数内部创建一个临时的、无状态的分配器实例，不会有内存占用
    Alloc alloc{};

    using Traits = std::allocator_traits<Alloc>;
    using Deleter = AllocatorDeleter<T, Alloc>;
    using UniquePtr = std::unique_ptr<T, Deleter>;

    // 仅分配内存
    T* raw_ptr = Traits::allocate(alloc, 1);
    // 原地构造对象
    Traits::construct(alloc, raw_ptr, std::forward<Args>(args)...);

    // 打包进 unique_ptr 并返回
    return UniquePtr(raw_ptr, Deleter(alloc));
}

} // namespace KzAlloc