#pragma once
#include <type_traits>
#include <utility>
#include <new>
#include <cassert>
#include "KzAlloc/ConcurrentAlloc.h"

namespace KzSTL {

class KzAny {
    static constexpr size_t kStorageSize = 24; // 足够放下 shared_ptr 或 weak_ptr
    static constexpr size_t kAlignment = alignof(std::max_align_t);

    struct VTable {
        void (*destroy)(void* storage);
        void (*move)(void* src, void* dst);
    };

    template <typename T>
    static constexpr VTable vtableForSmall = {
        [](void* storage) {
            std::launder(reinterpret_cast<T*>(storage))->~T();
        },
        [](void* src, void* dst) { 
            T* src_ptr = std::launder(reinterpret_cast<T*>(src));
            new (dst) T(std::move(*src_ptr));
            src_ptr->~T();
        }
    };

    template <typename T>
    static constexpr VTable vtableForLarge = {
        [](void* storage) { 
            T** ptr = std::launder(reinterpret_cast<T**>(storage));
            (*ptr)->~T();
            KzAlloc::free(*ptr, sizeof(T)); // 释放堆内存
        },
        [](void* src, void* dst) { 
            T** src_ptr = std::launder(reinterpret_cast<T**>(src));
            T** dst_ptr = static_cast<T**>(dst); // // dst 是新分配的，不需要 launder
            *dst_ptr = *src_ptr; // 拷贝指针
            *src_ptr = nullptr;  // 置空源指针，防止 double free
        }
    };

public:
    KzAny() noexcept : _vtable(nullptr) {}
    ~KzAny() { reset(); }

    KzAny(const KzAny&) = delete;
    KzAny& operator=(const KzAny&) = delete;
    
    KzAny(KzAny&& other) noexcept {
        movefrom(std::move(other));
    }

    KzAny& operator=(KzAny&& other) noexcept {
        if (this != &other) [[likely]] {
            if (_vtable) _vtable->destroy(&_storage);
            movefrom(std::move(other));
        }
        return *this;
    }

    template <typename T, typename std::enable_if_t<!std::is_same_v<std::decay_t<T>, KzAny>>>
    KzAny(T&& value) {
        using DecayedT = std::decay_t<T>;
        static_assert(std::is_move_constructible_v<DecayedT>,
            "FATAL: KzAny require the T to be move constructible.");
         static_assert(std::is_nothrow_move_constructible_v<DecayedT>,
            "FATAL: The T passed to KzAny MUST be noexcept move constructible! "
            "Do not capture large objects by value, use std::unique_ptr or raw pointers.");
        
        if constexpr (sizeof(DecayedT) <= kStorageSize && alignof(DecayedT) <= kAlignment) {
            new (&_storage) DecayedT(std::forward<T>(value));
            _vtable = &vtableForSmall<DecayedT>;
        } else {
            void* mem = KzAlloc::malloc(sizeof(DecayedT));
            if (!mem) [[unlikely]] KzAlloc::handleOOM();
            auto* ptr = new (mem) DecayedT(std::forward<T>(value));
            new (&_storage) DecayedT*(ptr);
            _vtable = &vtableForLarge<DecayedT>;
        }
    }

    void reset() noexcept {
        if (_vtable) [[likely]] {
            _vtable->destroy(&_storage);
            _vtable = nullptr;
        }
    }

    bool has_value() const noexcept { return _vtable != nullptr; }

    // 极速强转 (去掉了 RTTI 检查，调用者必须自己保证类型正确)
    template <typename T>
    T& cast() {
        assert(has_value());
        if constexpr (sizeof(T) <= kStorageSize && alignof(T) <= kAlignment) {
            return *std::launder(reinterpret_cast<T*>(&_storage));
        } else {
            return **std::launder(reinterpret_cast<T**>(&_storage));
        }
    }

private:
    void movefrom(KzAny&& other) noexcept {
        if (other._vtable) [[likely]] {
            _vtable = other._vtable;
            _vtable->move(&other._storage, &_storage);
            other._vtable = nullptr;
        } else {
            _vtable = nullptr;
        }
    }

private:
    const VTable* _vtable;
    alignas(kAlignment) std::byte _storage[kStorageSize];
};

} // namespace KzSTL