#pragma once

#include <utility>
#include <type_traits>
#include <new>
#include <cassert>
#include <concepts>
#include <cstddef>
#include "KzAlloc/ConcurrentAlloc.h"

namespace KzSTL {

// 主模板声明
// 强制用户使用偏特化版本，例如 Function<void()> 或 Function<void() const>
template<typename Signature, size_t InlineSize = 48, size_t Alignment = alignof(void*)>
class Function;

/**
 * @brief 高性能、可拷贝、泛型函数包装器 (替代 std::function)
 * 适用于需要 1:N 分发的函数，支持拷贝和 Mutable Lambda
 */

// 非 Const 偏特化版本 (允许 Mutable Lambda)
// 签名: R(Args...)
template<typename R, typename... Args, size_t InlineSize, size_t Alignment>
class Function<R(Args...), InlineSize, Alignment> {
public:
    static_assert(InlineSize % alignof(void*) == 0, "InlineSize must be a multiple of pointer size to avoid padding");
    static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be a power of 2");
    static constexpr size_t kStorageSize = InlineSize;
    static constexpr size_t kAlignment = Alignment;

    // --- 构造与析构 ---
    Function() noexcept : _call(nullptr), _vtable(nullptr) {}
    Function(std::nullptr_t) noexcept : _call(nullptr), _vtable(nullptr) {}

    ~Function() noexcept {
        if (_vtable) {
            _vtable->destroy(&_storage);
            // 不置空 _vtable, 不防御 Double Free 非法行为
        }
    }

    // --- 移动语义 ---
    Function(Function&& other) noexcept {
        moveFrom(std::move(other));
    }

    Function& operator=(Function&& other) noexcept {
        if (this != &other) [[likely]] {
            if (_vtable) _vtable->destroy(&_storage);
            moveFrom(std::move(other));
        }
        return *this;
    }

    // --- 拷贝语义 ---
    Function(const Function& other) noexcept {
        copyFrom(other);
    }

    Function& operator=(const Function& other) noexcept {
        if (this != &other) [[likely]] {
            if (_vtable) _vtable->destroy(&_storage);
            copyFrom(other);
        }
        return *this;
    }

    // --- 模板构造 ---
    template<typename F>
        requires (!std::same_as<std::decay_t<F>, Function>) && 
            std::copy_constructible<std::decay_t<F>> &&
            std::is_nothrow_constructible_v<std::decay_t<F>, F> &&
            std::is_nothrow_copy_constructible_v<std::decay_t<F>> && 
            // 要求 F& 是可调用的 (允许 mutable lambda)
            std::is_nothrow_invocable_r_v<R, std::decay_t<F>, Args...>
    Function(F&& f) noexcept {
        using DecayedF = std::decay_t<F>;

        if constexpr (sizeof(DecayedF) <= kStorageSize && alignof(DecayedF) <= kAlignment) {
            new (&_storage) DecayedF(std::forward<F>(f));
            _call = [](void* storage, Args... args) noexcept -> R {
                DecayedF* ptr = std::launder(reinterpret_cast<DecayedF*>(storage));
                return (*ptr)(std::forward<Args>(args)...);
            };
            _vtable = &vtableForSmall<DecayedF>;
        } else {
            // Heap Allocation
            void* mem = nullptr;
            if constexpr (alignof(DecayedF) > KzAlloc::kMinAlignment) {
                mem = KzAlloc::malloc_aligned(sizeof(DecayedF), alignof(DecayedF));
            }
            else {
                mem = KzAlloc::malloc(sizeof(DecayedF));
            }
            if (!mem) [[unlikely]] KzAlloc::handleOOM();
            
            DecayedF* ptr = new (mem) DecayedF(std::forward<F>(f)); // 不抛异常，不需要 RAII 保护 mem
            // 存指针到 storage
            new (&_storage) DecayedF*(ptr);
            _call =[](void* storage, Args... args) noexcept -> R {
                DecayedF** ptr_to_ptr = std::launder(reinterpret_cast<DecayedF**>(storage));
                return (**ptr_to_ptr)(std::forward<Args>(args)...);
            };
            _vtable = &vtableForLarge<DecayedF>;
        }
    }

    // --- 调用接口 ---
    explicit operator bool() const noexcept {
        return _vtable != nullptr;
    }

    // 非 const 的 operator()
    R operator()(Args... args) noexcept {
        assert(_call && "Calling empty Function");
        return _call(&_storage, std::forward<Args>(args)...); // 契约：外界函数不应抛异常
    }

private:
    struct VTable {
        void (*move)(void* src, void* dst) noexcept;
        void (*clone)(const void* src, void* dst) noexcept;
        void (*destroy)(void* storage) noexcept;
    };

    // --- Small VTable (栈上存储) ---
    template <typename F>
    static constexpr VTable vtableForSmall = {
        [](void* src, void* dst) noexcept {
            F* src_ptr = std::launder(reinterpret_cast<F*>(src));
            new (dst) F(std::move(*src_ptr));
            src_ptr->~F();
        },
        [](const void* src, void* dst) noexcept {
            const F* const src_ptr = std::launder(reinterpret_cast<const F*>(src));
            new (dst) F(*src_ptr);
        },
        [](void* storage) noexcept {
            std::launder(reinterpret_cast<F*>(storage))->~F();
        }
    };

    // --- Large VTable (堆上存储) ---
    template <typename F>
    static constexpr VTable vtableForLarge = {
        [](void* src, void* dst) noexcept {
            F** src_ptr = std::launder(reinterpret_cast<F**>(src));
            F** dst_ptr = static_cast<F**>(dst);
            *dst_ptr = *src_ptr; // 移动指针所有权
            *src_ptr = nullptr;
        },
        [](const void* src, void* dst) noexcept {
            F* const* src_ptr = std::launder(reinterpret_cast<F* const*>(src));
            
            void* mem = nullptr;
            if constexpr (alignof(F) > KzAlloc::kMinAlignment) {
                mem = KzAlloc::malloc_aligned(sizeof(F), alignof(F));
            }
            else {
                mem = KzAlloc::malloc(sizeof(F));
            }
            if (!mem) [[unlikely]] KzAlloc::handleOOM();

            F* new_ptr = new (mem) F(**src_ptr);

            new (dst) F*(new_ptr);
        },
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

    void copyFrom(const Function& other) noexcept {
        if (other._vtable) [[likely]] {
            _call = other._call;
            _vtable = other._vtable;
            _vtable->clone(&other._storage, &_storage); // 契约：拷贝函数不允许抛异常
        } else {
            _call = nullptr;
            _vtable = nullptr;
        }
    }

    void moveFrom(Function&& other) noexcept {
        if (other._vtable) [[likely]] {
            _call = other._call;
            _vtable = other._vtable;
            _vtable->move(&other._storage, &_storage);
            other._call = nullptr;
            other._vtable = nullptr;
        } else {
            _call = nullptr;
            _vtable = nullptr;
        }
    }

private:
    alignas(kAlignment) std::byte _storage[kStorageSize];
    R (*_call)(void* storage, Args... args) noexcept;
    const VTable* _vtable; 
};

// Const 偏特化版本 (严格禁止 Mutable Lambda)
// 签名: R(Args...) const
template<typename R, typename... Args, size_t InlineSize, size_t Alignment>
class Function<R(Args...) const, InlineSize, Alignment> {
public:
    static_assert(InlineSize % alignof(void*) == 0, "InlineSize must be a multiple of pointer size");
    static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be a power of 2");
    static constexpr size_t kStorageSize = InlineSize;
    static constexpr size_t kAlignment = Alignment;

    Function() noexcept : _call(nullptr), _vtable(nullptr) {}
    Function(std::nullptr_t) noexcept : _call(nullptr), _vtable(nullptr) {}

    ~Function() noexcept {
        if (_vtable) _vtable->destroy(&_storage);
    }

    Function(Function&& other) noexcept { moveFrom(std::move(other)); }
    Function& operator=(Function&& other) noexcept {
        if (this != &other) [[likely]] {
            if (_vtable) _vtable->destroy(&_storage);
            moveFrom(std::move(other));
        }
        return *this;
    }

    Function(const Function& other) noexcept { copyFrom(other); }
    Function& operator=(const Function& other) noexcept {
        if (this != &other) [[likely]] {
            if (_vtable) _vtable->destroy(&_storage);
            copyFrom(other);
        }
        return *this;
    }

    template<typename F>
        requires (!std::same_as<std::decay_t<F>, Function>) && 
                 std::copy_constructible<std::decay_t<F>> &&
                 std::is_nothrow_constructible_v<std::decay_t<F>, F> &&
                 std::is_nothrow_copy_constructible_v<std::decay_t<F>> && 
                 // 要求 const F& 是可调用的 (不允许 mutable lambda)
                 std::is_nothrow_invocable_r_v<R, const std::decay_t<F>&, Args...>
    Function(F&& f) noexcept {
        using DecayedF = std::decay_t<F>;

        if constexpr (sizeof(DecayedF) <= kStorageSize && alignof(DecayedF) <= kAlignment) {
            new (&_storage) DecayedF(std::forward<F>(f));
            // _call 接收 const void*
            _call =[](const void* storage, Args... args) noexcept -> R {
                const DecayedF* ptr = std::launder(reinterpret_cast<const DecayedF*>(storage));
                return (*ptr)(std::forward<Args>(args)...);
            };
            _vtable = &vtableForSmall<DecayedF>;
        } else {
            void* mem = nullptr;
            if constexpr (alignof(DecayedF) > KzAlloc::kMinAlignment) {
                mem = KzAlloc::malloc_aligned(sizeof(DecayedF), alignof(DecayedF));
            }
            else {
                mem = KzAlloc::malloc(sizeof(DecayedF));
            }
            if (!mem) [[unlikely]] KzAlloc::handleOOM();
            
            DecayedF* ptr = new (mem) DecayedF(std::forward<F>(f));
            new (&_storage) DecayedF*(ptr);
            
            _call =[](const void* storage, Args... args) noexcept -> R {
                DecayedF* const* ptr_to_ptr = std::launder(reinterpret_cast<DecayedF* const*>(storage));
                return (**ptr_to_ptr)(std::forward<Args>(args)...);
            };
            _vtable = &vtableForLarge<DecayedF>;
        }
    }

    explicit operator bool() const noexcept { return _vtable != nullptr; }

    // Const 的 operator()
    R operator()(Args... args) const noexcept {
        assert(_call && "Calling empty Function");
        // 传递 &_storage 时，由于 this 是 const，&_storage 自动推导为 const std::byte*
        return _call(&_storage, std::forward<Args>(args)...);
    }

private:
    struct VTable {
        void (*move)(void* src, void* dst) noexcept;
        void (*clone)(const void* src, void* dst) noexcept;
        void (*destroy)(void* storage) noexcept;
    };

    // VTable 的实现与非 Const 版本完全一致，因为 move/clone/destroy 依然需要修改 storage
    template <typename F>
    static constexpr VTable vtableForSmall = {
        [](void* src, void* dst) noexcept {
            F* src_ptr = std::launder(reinterpret_cast<F*>(src));
            new (dst) F(std::move(*src_ptr));
            src_ptr->~F();
        },
        [](const void* src, void* dst) noexcept {
            const F* src_ptr = std::launder(reinterpret_cast<const F*>(src));
            new (dst) F(*src_ptr);
        },
        [](void* storage) noexcept {
            std::launder(reinterpret_cast<F*>(storage))->~F();
        }
    };

    template <typename F>
    static constexpr VTable vtableForLarge = {
        [](void* src, void* dst) noexcept {
            F** src_ptr = std::launder(reinterpret_cast<F**>(src));
            F** dst_ptr = static_cast<F**>(dst);
            *dst_ptr = *src_ptr;
            *src_ptr = nullptr;
        },
        [](const void* src, void* dst) noexcept {
            F* const* src_ptr = std::launder(reinterpret_cast<F* const*>(src));
            void* mem = nullptr;
            if constexpr (alignof(F) > KzAlloc::kMinAlignment) {
                mem = KzAlloc::malloc_aligned(sizeof(F), alignof(F));
            }
            else {
                mem = KzAlloc::malloc(sizeof(F));
            }
            if (!mem) [[unlikely]] KzAlloc::handleOOM();
            F* new_ptr = new (mem) F(**src_ptr);
            new (dst) F*(new_ptr);
        },
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

    void copyFrom(const Function& other) noexcept {
        if (other._vtable) [[likely]] {
            _call = other._call;
            _vtable = other._vtable;
            _vtable->clone(&other._storage, const_cast<std::byte*>(&_storage[0])); // 仅在内部拷贝时去 const
        } else {
            _call = nullptr;
            _vtable = nullptr;
        }
    }

    void moveFrom(Function&& other) noexcept {
        if (other._vtable) [[likely]] {
            _call = other._call;
            _vtable = other._vtable;
            _vtable->move(const_cast<std::byte*>(&other._storage[0]), const_cast<std::byte*>(&_storage[0]));
            other._call = nullptr;
            other._vtable = nullptr;
        } else {
            _call = nullptr;
            _vtable = nullptr;
        }
    }

private:
    alignas(kAlignment) std::byte _storage[kStorageSize];
    R (*_call)(const void* storage, Args... args) noexcept;
    const VTable* _vtable;
};

} // namespace KzSTL