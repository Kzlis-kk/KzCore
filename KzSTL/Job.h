#pragma once

#include <utility>
#include <type_traits>
#include <new>
#include <cassert>
#include <cstddef>
#include <concepts>
#include "KzAlloc/ConcurrentAlloc.h"

namespace KzSTL {

// 前向声明
template<typename Signature, size_t InlineSize = 56, size_t Alignment = alignof(void*)>
class Job;

/**
 * @brief 高性能、Move-Only、泛型函数包装器 (替代 std::move_only_function)
 * 用于单使用者回调，以及不支持拷贝的任务（比如包含 unique_ptr 的任务）
 * 注意: 在对齐数大于 8 时，虚表加上其 Padding 会等于对齐数，此时 Alignment + InlineSize（强制无 pandding） 才是实际大小
 */
template<typename R, typename... Args, size_t InlineSize, size_t Alignment>
class Job<R(Args...), InlineSize, Alignment> {
public:
    // InlineSize 必须是对齐大小的整数倍，防止尾部 Padding
    static_assert(InlineSize % Alignment == 0, "InlineSize must be a multiple of Alignment");
    // 对齐大小必须是 2 的幂
    static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be a power of 2");

    static constexpr size_t kStorageSize = InlineSize;
    static constexpr size_t kAlignment = Alignment;

    Job() noexcept : _vtable(nullptr) {}
    Job(std::nullptr_t) noexcept : _vtable(nullptr) {}

    Job(Job&& other) noexcept {
        movefrom(std::move(other));
    }

    Job& operator=(Job&& other) noexcept {
        if (this != &other) [[likely]] {
            if (_vtable) _vtable->destroy(&_storage);
            movefrom(std::move(other));
        }
        return *this;
    }

    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    // 核心构造函数
    template<typename F>
        requires (!std::same_as<std::remove_cvref_t<F>, Job>) && 
            std::move_constructible<std::remove_cvref_t<F>> &&
            std::is_nothrow_constructible_v<std::remove_cvref_t<F>, F> && 
            std::is_nothrow_invocable_r_v<R, std::remove_cvref_t<F>, Args...>
    Job(F&& f) noexcept {
        using DecayedF = std::remove_cvref_t<F>;

        // 判定条件：大小合适 且 对齐要求不超过模板指定的 Alignment
        if constexpr (sizeof(DecayedF) <= kStorageSize && alignof(DecayedF) <= kAlignment) {
            new (&_storage) DecayedF(std::forward<F>(f));
            _vtable = &vtableForSmall<DecayedF>;
        } else {
            // 在堆上分配，并将指针存入 _storage
            void* mem = nullptr;
            if constexpr (alignof(DecayedF) > KzAlloc::kMinAlignment) {
                mem = KzAlloc::malloc_aligned(sizeof(DecayedF), alignof(DecayedF));
            }
            else {
                mem = KzAlloc::malloc(sizeof(DecayedF));
            }
            if (!mem) [[unlikely]] KzAlloc::handleOOM();

            auto* ptr = new (mem) DecayedF(std::forward<F>(f));

            new (&_storage) DecayedF*(ptr); // 存指针
            _vtable = &vtableForLarge<DecayedF>;
        }
    }

    ~Job() noexcept {
        if (_vtable) {
            _vtable->destroy(&_storage);
            // 不置空 _vtable, 不防御 Double Free 非法行为
        }
    }

    explicit operator bool() const noexcept {
        return _vtable != nullptr;
    }

    // 调用接口：支持传参和返回值
    R operator()(Args... args) noexcept {
        assert(_vtable && "Calling empty Job");
        return _vtable->call(&_storage, std::forward<Args>(args)...); // 契约：外界函数不应抛异常
    }

private:
    struct VTable {
        R (*call)(void* storage, Args... args) noexcept;
        void (*move)(void* src, void* dst) noexcept;
        void (*destroy)(void* storage) noexcept;
    };

    // --- VTable for Small Objects (直接存对象) ---
    template <typename F>
    static constexpr VTable vtableForSmall = {
        // Call: 直接强转 storage 为对象
        [](void* storage, Args... args) noexcept -> R {
            F* ptr = std::launder(reinterpret_cast<F*>(storage));
            return (*ptr)(std::forward<Args>(args)...);
        },
        // Move: 调用对象的移动构造
        [](void* src, void* dst) noexcept {
            F* src_ptr = std::launder(reinterpret_cast<F*>(src));
            new (dst) F(std::move(*src_ptr));
            src_ptr->~F();
        },
        // Destroy: 调用对象的析构
        [](void* storage) noexcept {
           std::launder(reinterpret_cast<F*>(storage))->~F();
        }
    };

    // --- VTable for Large Objects (存指针) ---
    template <typename F>
    static constexpr VTable vtableForLarge = {
        // Call: storage 里存的是 F*，需要两层解引用
        [](void* storage, Args... args) noexcept -> R {
            F** ptr_to_ptr = std::launder(reinterpret_cast<F**>(storage));
            return (**ptr_to_ptr)(std::forward<Args>(args)...);
        },
        // Move: 只是移动指针 (8字节)，不需要移动堆上的对象
        [](void* src, void* dst) noexcept {
            F** src_ptr = std::launder(reinterpret_cast<F**>(src));
            F** dst_ptr = static_cast<F**>(dst); // dst 是新分配的，不需要 launder
            *dst_ptr = *src_ptr; // 拷贝指针
            *src_ptr = nullptr;  // 置空源指针，防止 double free
        },
        // Destroy: delete 指针
        [](void* storage) noexcept {
            F** ptr = std::launder(reinterpret_cast<F**>(storage));
            (*ptr)->~F();
            if constexpr (alignof(F) > KzAlloc::kMinAlignment) {
                KzAlloc::free_aligned(*ptr, sizeof(F), alignof(F));
            }
            else {
                KzAlloc::free(*ptr, sizeof(F));
            }
        }
    };

    void movefrom(Job&& other) noexcept {
        if (other._vtable) [[likely]] {
            _vtable = other._vtable;
            _vtable->move(&other._storage, &_storage);
            other._vtable = nullptr;
        } else {
            _vtable = nullptr;
        }
    }

private:
    alignas(kAlignment) mutable std::byte _storage[kStorageSize];
    const VTable* _vtable;
    
};

} // namespace KzSTL