#pragma once

#include <new>      
#include <utility>   
#include <stdexcept>
#include <cassert>
#include <type_traits>
#include <memory>

namespace KzSTL {

template<typename E>
struct Unexpected {
    E val;
    constexpr explicit Unexpected(E e) : val(std::move(e)) {}
};

template<typename E>
constexpr Unexpected<E> make_unexpected(E e) {
    return Unexpected<E>(std::move(e));
}

/**
 * @brief Expected: 零开销的 Result 抽象 (C++20 标准)
 */
template<typename T, typename E>
class [[nodiscard]] Expected {
public:
    // === 1. 构造函数 (引入条件 noexcept) ===

    constexpr Expected(const T& val) noexcept(std::is_nothrow_copy_constructible_v<T>) 
        : _hasValue(true) {
        std::construct_at(&_val, val); // C++20 替代 placement new
    }

    constexpr Expected(T&& val) noexcept(std::is_nothrow_move_constructible_v<T>) 
        : _hasValue(true) {
        std::construct_at(&_val, std::move(val));
    }

    constexpr Expected(const Unexpected<E>& err) noexcept(std::is_nothrow_copy_constructible_v<E>) 
        : _hasValue(false) {
        std::construct_at(&_err, err.val);
    }

    constexpr Expected(Unexpected<E>&& err) noexcept(std::is_nothrow_move_constructible_v<E>) 
        : _hasValue(false) {
        std::construct_at(&_err, std::move(err.val));
    }

    // === 2. 拷贝/移动构造 ===

    constexpr Expected(const Expected& other) noexcept(
        std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_constructible_v<E>) 
        : _hasValue(other._hasValue) {
        if (_hasValue) {
            std::construct_at(&_val, other._val);
        } else {
            std::construct_at(&_err, other._err);
        }
    }

    constexpr Expected(Expected&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>) 
        : _hasValue(other._hasValue) {
        if (_hasValue) {
            std::construct_at(&_val, std::move(other._val));
        } else {
            std::construct_at(&_err, std::move(other._err));
        }
    }

    // === 3. 赋值运算符 (核心性能优化区) ===

    constexpr Expected& operator=(const Expected& other) noexcept(
        std::is_nothrow_copy_assignable_v<T> && std::is_nothrow_copy_constructible_v<T> &&
        std::is_nothrow_copy_assignable_v<E> && std::is_nothrow_copy_constructible_v<E>) 
    {
        if (this == &other) [[unlikely]] return *this;

        if (_hasValue == other._hasValue) {
            // 状态未改变：直接调用赋值运算符，复用底层 Capacity！
            if (_hasValue) {
                _val = other._val; 
            } else {
                _err = other._err;
            }
        } else {
            // 状态改变：需要析构旧类型，构造新类型 (需保证强异常安全)
            if (_hasValue) { // T -> E
                E temp(other._err); // 1. 先构造临时对象 (如果抛异常，this 状态不变)
                std::destroy_at(&_val); // 2. 析构旧对象
                std::construct_at(&_err, std::move(temp)); // 3. 移动构造新对象 (通常 noexcept)
                _hasValue = false;
            } else { // E -> T
                T temp(other._val);
                std::destroy_at(&_err);
                std::construct_at(&_val, std::move(temp));
                _hasValue = true;
            }
        }
        return *this;
    }

    constexpr Expected& operator=(Expected&& other) noexcept(
        std::is_nothrow_move_assignable_v<T> && std::is_nothrow_move_constructible_v<T> &&
        std::is_nothrow_move_assignable_v<E> && std::is_nothrow_move_constructible_v<E>) 
    {
        if (this == &other) [[unlikely]] return *this;

        if (_hasValue == other._hasValue) {
            if (_hasValue) {
                _val = std::move(other._val); // 复用 Capacity 并 Move
            } else {
                _err = std::move(other._err);
            }
        } else {
            if (_hasValue) { // T -> E
                E temp(std::move(other._err)); 
                std::destroy_at(&_val);
                std::construct_at(&_err, std::move(temp));
                _hasValue = false;
            } else { // E -> T
                T temp(std::move(other._val));
                std::destroy_at(&_err);
                std::construct_at(&_val, std::move(temp));
                _hasValue = true;
            }
        }
        return *this;
    }

    // === 4. 析构函数 ===
    
    // C++20 允许 constexpr 析构函数
    constexpr ~Expected() noexcept {
        destroy_current();
    }

    // === 5. 状态查询与数据访问 ===

    constexpr bool has_value() const noexcept { return _hasValue; }
    constexpr explicit operator bool() const noexcept { return _hasValue; }

    constexpr T& value() & noexcept { 
        assert(_hasValue);
        return _val;
    }
    constexpr const T& value() const & noexcept { 
        assert(_hasValue); 
        return _val; 
    }

    constexpr E& error() & noexcept { 
        assert(!_hasValue); 
        return _err; 
    }
    constexpr const E& error() const & noexcept { 
        assert(!_hasValue); 
        return _err; 
    }

    // === 6. value_or (按值传递 + std::move) ===
    
    constexpr T value_or(T default_val) const & noexcept(std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T>) {
        // 如果有值，拷贝 _val；如果无值，移动 default_val
        return _hasValue ? _val : std::move(default_val);
    }

    constexpr T value_or(T default_val) && noexcept(std::is_nothrow_move_constructible_v<T>) {
        // 如果本身是右值，连 _val 也一起 move 掉
        return _hasValue ? std::move(_val) : std::move(default_val);
    }

private:
    constexpr void destroy_current() noexcept {
        if (_hasValue) {
            std::destroy_at(&_val); // C++20 安全析构
        } else {
            std::destroy_at(&_err);
        }
    }

    union {
        T _val;
        E _err;
    };
    bool _hasValue;
};

/**
 * @brief Expected 的 void 偏特化版本
 * 用于只关心成功与否，不产生实际返回值的场景 (如 RPC 的 Ping/Pong 或纯写操作)
 */
template<typename E>
class [[nodiscard]] Expected<void, E> {
public:
    // === 1. 构造函数 ===

    // 默认构造即代表 Success
    constexpr Expected() noexcept 
        : _dummy(), _hasValue(true) {}

    // 从错误构造 (Failure)
    constexpr Expected(const Unexpected<E>& err) noexcept(std::is_nothrow_copy_constructible_v<E>) 
        : _hasValue(false) {
        std::construct_at(&_err, err.val);
    }

    constexpr Expected(Unexpected<E>&& err) noexcept(std::is_nothrow_move_constructible_v<E>) 
        : _hasValue(false) {
        std::construct_at(&_err, std::move(err.val));
    }

    // === 2. 拷贝/移动构造 ===

    constexpr Expected(const Expected& other) noexcept(std::is_nothrow_copy_constructible_v<E>) 
        : _hasValue(other._hasValue) {
        if (_hasValue) {
            std::construct_at(&_dummy); // 激活 dummy 成员
        } else {
            std::construct_at(&_err, other._err);
        }
    }

    constexpr Expected(Expected&& other) noexcept(std::is_nothrow_move_constructible_v<E>) 
        : _hasValue(other._hasValue) {
        if (_hasValue) {
            std::construct_at(&_dummy);
        } else {
            std::construct_at(&_err, std::move(other._err));
        }
    }

    // === 3. 赋值运算符 (强异常安全) ===

    constexpr Expected& operator=(const Expected& other) noexcept(
        std::is_nothrow_copy_assignable_v<E> && std::is_nothrow_copy_constructible_v<E>) 
    {
        if (this == &other) [[unlikely]] return *this;

        if (_hasValue == other._hasValue) {
            if (!_hasValue) {
                _err = other._err; // 状态未变，复用 E 的 capacity
            }
        } else {
            if (_hasValue) { // void -> E
                E temp(other._err); // 强异常安全：先构造 temp
                std::destroy_at(&_dummy);
                std::construct_at(&_err, std::move(temp));
                _hasValue = false;
            } else { // E -> void
                std::destroy_at(&_err);
                std::construct_at(&_dummy);
                _hasValue = true;
            }
        }
        return *this;
    }

    constexpr Expected& operator=(Expected&& other) noexcept(
        std::is_nothrow_move_assignable_v<E> && std::is_nothrow_move_constructible_v<E>) 
    {
        if (this == &other) [[unlikely]] return *this;

        if (_hasValue == other._hasValue) {
            if (!_hasValue) {
                _err = std::move(other._err);
            }
        } else {
            if (_hasValue) { // void -> E
                E temp(std::move(other._err));
                std::destroy_at(&_dummy);
                std::construct_at(&_err, std::move(temp));
                _hasValue = false;
            } else { // E -> void
                std::destroy_at(&_err);
                std::construct_at(&_dummy);
                _hasValue = true;
            }
        }
        return *this;
    }

    // === 4. 析构函数 ===
    
    constexpr ~Expected() noexcept {
        if (!_hasValue) {
            std::destroy_at(&_err);
        } else {
            std::destroy_at(&_dummy);
        }
    }

    // === 5. 状态查询与数据访问 ===

    constexpr bool has_value() const noexcept { return _hasValue; }
    constexpr explicit operator bool() const noexcept { return _hasValue; }

    // value() 返回 void，仅作断言检查
    constexpr void value() const noexcept { 
        assert(_hasValue); 
    }

    constexpr E& error() & noexcept { 
        assert(!_hasValue); 
        return _err; 
    }
    constexpr const E& error() const & noexcept { 
        assert(!_hasValue); 
        return _err; 
    }

    // 移除了 value_or()，因为对于 void 类型没有意义

private:
    // 占位符结构体，用于维持 union 的合法性
    struct Dummy {};

    union {
        Dummy _dummy;
        E _err;
    };
    bool _hasValue;
};

} // namespace KzSTL