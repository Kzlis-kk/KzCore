#pragma once
#include <atomic>
#include <thread>
#include <new>

// 引入 PAUSE 指令
#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace KzSTL {

#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    constexpr size_t kCacheLineSize = 64;
#endif

/**
 * @brief 高性能混合自旋锁
 * 
 * 基于 C++20 std::atomic + 状态机 实现，结合了用户态自旋与内核态阻塞（Futex）的优势：
 * 1. 快速路径：直接 CAS,使用硬件指令
 * 2. 自旋路径：使用 PAUSE 指令进行短时间自旋，降低 CPU 功耗并避免内存顺序冲突导致的流水线冲刷。
 * 3. 慢速路径：当自旋超过阈值后，利用 C++20 的 atomic::wait() 陷入内核态阻塞，避免长竞争时死循环耗尽 CPU 资源。
 * 
 */
class alignas(kCacheLineSize) SpinMutex {
public:
    explicit SpinMutex() = default;
    
    SpinMutex(const SpinMutex&) = delete;
    SpinMutex& operator=(const SpinMutex&) = delete;

    void lock() noexcept {
        // 快速路径：直接尝试 CAS
        uint8_t expected = 0;
        if (_state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) [[likely]] {
            return;
        }

        // 自旋路径 (用户态)
        for (int i = 0; i < 64; ++i) {
            if (!_state.load(std::memory_order_relaxed)) {
                expected = 0;
                if (_state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
                    return;
                }
            }
#if defined(_MSC_VER)
            _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
            __builtin_ia32_pause();
#endif
        }
            
        // 慢速路径 (陷入内核)
        // 将状态强制置为 2，并等待状态不再是 2
        while (_state.exchange(2, std::memory_order_acquire) != 0) {
            _state.wait(2, std::memory_order_relaxed);
        }
    }

    void unlock() noexcept {
        // 将状态置回 0。如果之前是 2 (有等待者)，则需要 notify
        if (_state.exchange(0, std::memory_order_release) == 2) {
            _state.notify_one();
        }
    }

    // 尝试获取锁
    [[nodiscard]] bool try_lock() noexcept {
        uint8_t expected = 0;
        return _state.compare_exchange_strong(expected, 1, std::memory_order_acquire);
    }

private:
    // 0: Free, 1: Locked, 2: Locked & Waiting
    std::atomic<uint8_t> _state{0};
};

} // namespace KzSTL