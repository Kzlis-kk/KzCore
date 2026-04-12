#pragma once

#include <coroutine>
#include <exception>
#include <utility>
#include <cassert>
#include <atomic>
#include <type_traits>
#include "KzAlloc/ConcurrentAlloc.h"

namespace KzSTL {

// 注意，内部判断是否可以 resume 的逻辑是 协程句柄自带的 done()，只有协程执行完 done() 才变成 true
// 所以，清不要二次启动，包括中途如果挂起然后手动resume
template<typename T = void> class Task;

namespace detail {

    // ========================================================================
    // 1. PromiseBase: 公共逻辑 (内存池、异常、同步状态、生命周期)
    // ========================================================================
    struct PromiseBase {
        std::coroutine_handle<> _continuation; 
        std::exception_ptr _exception;
        std::atomic<int> _state{0}; // 0: running, 1: done
        bool _auto_destroy = false; // detach 标志

        // 内存池重载
        static void* operator new(size_t size) {
            void* ptr = KzAlloc::malloc(size);
            if (!ptr) [[unlikely]] KzAlloc::handleOOM();
            return ptr;
        }
        static void operator delete(void* ptr, size_t size) {
            KzAlloc::free(ptr, size);
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }
            
            template <typename PromiseType>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
                PromiseBase& promise = h.promise(); // 安全向上转型
                
                // 1. 标记完成，唤醒 get() 的等待者
                promise._state.store(1, std::memory_order_release);
                promise._state.notify_all();

                // 2. 如果是 detach 状态，自杀
                if (promise._auto_destroy) {
                    h.destroy();
                    return std::noop_coroutine();
                }

                // 3. 对称转移：切回父协程
                if (promise._continuation) {
                    return promise._continuation;
                }

                // 4. 挂起，等待 Task 析构
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }
        void unhandled_exception() { _exception = std::current_exception(); }
    };

    // ========================================================================
    // 2. Promise<T>: 处理返回值的差异 (利用模板特化)
    // ========================================================================
    
    // 通用版本 (T != void)
    template<typename T>
    struct Promise : PromiseBase {
        T _value; // 存储返回值

        Task<T> get_return_object();

        template <typename U>
        requires (std::convertible_to<U, T>)
        void return_value(U&& val) { _value = std::forward<U>(val); }
    };

    // 特化版本 (T == void)
    template<>
    struct Promise<void> : PromiseBase {
        Task<void> get_return_object();

        void return_void() {} // void 协程没有 _value 成员
    };

} // namespace detail

// ============================================================================
// 3. 泛型 Task<T>: 统一接口
// ============================================================================
template<typename T>
class Task {
public:
    // 自动选择对应的 Promise 类型
    using promise_type = detail::Promise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    explicit Task(Handle h) : _handle(h) {}
    
    ~Task() { 
        if (_handle) _handle.destroy(); 
    }

    // Move Only
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& other) noexcept : _handle(other._handle) { other._handle = nullptr; }
    Task& operator=(Task&& other) noexcept {
        if (this != &other) [[likely]] {
            if (_handle) _handle.destroy();
            _handle = other._handle;
            other._handle = nullptr;
        }
        return *this;
    }

    // --- Awaitable 接口 ---
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        _handle.promise()._continuation = caller;
        return _handle;
    }

    // 根据 T 是否为 void 决定返回值
    decltype(auto) await_resume() {
        if (_handle.promise()._exception) [[unlikely]] {
            std::rethrow_exception(_handle.promise()._exception);
        }
        // if constexpr 编译期分支：如果是 void，什么都不返
        if constexpr (!std::is_void_v<T>) {
            return std::move(_handle.promise()._value);
        }
        else {
            return; // 显式 return 避免 decltype(auto) 推导歧义
        }
    }

    // --- 顶层接口 ---

    void start() {
        if (_handle && !_handle.done()) [[likely]] _handle.resume();
    }

    // 根据 T 是否为 void 决定返回值
    decltype(auto) get() {
        assert(_handle);
        if (!_handle.done()) _handle.resume();

        auto& state = _handle.promise()._state;
        while (state.load(std::memory_order_acquire) == 0) {
            state.wait(0, std::memory_order_relaxed);
        }

        if (_handle.promise()._exception) [[unlikely]] {
            std::rethrow_exception(_handle.promise()._exception);
        }

        if constexpr (!std::is_void_v<T>) {
            return std::move(_handle.promise()._value);
        }
    }

    // 限制只有 Task<void> 才能 detach
    void detach() {
        static_assert(std::is_void_v<T>, "Only Task<void> can be detached!");
        
        if (_handle) [[likely]] {
            _handle.promise()._auto_destroy = true;
            _handle = nullptr;
        }
    }

    void start_detached() {
        static_assert(std::is_void_v<T>, "Only Task<void> can be detached!");
        if (_handle) [[likely]] {
            _handle.promise()._auto_destroy = true; // 启动前就设置好自毁标志
            auto h = _handle;
            _handle = nullptr; // 放弃所有权
            if (!h.done()) h.resume(); // 点火
        }
    }

private:
    Handle _handle;
};

// ============================================================================
// 4. 延迟实现的 get_return_object (因为 Task 定义在 Promise 之后)
// ============================================================================
namespace detail {
    template<typename T>
    Task<T> Promise<T>::get_return_object() {
        return Task<T>{std::coroutine_handle<Promise<T>>::from_promise(*this)};
    }

    inline Task<void> Promise<void>::get_return_object() {
        return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
    }
}

} // namespace KzSTL