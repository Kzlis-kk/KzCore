#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <cassert>

#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    constexpr size_t kCacheLineSize = 64;
#endif

namespace KzLog {

/**
 * @brief 线程局部的无锁 SPSC 环形缓冲区
 */
template <size_t CAPACITY = 1024 * 1024> // 默认 1MB，必须是 2 的幂
class ThreadLocalBuffer {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "Capacity must be a power of 2");
    static constexpr uint64_t MASK = CAPACITY - 1;

public:
    ThreadLocalBuffer() noexcept = default;
    ~ThreadLocalBuffer() noexcept = default;

    // 禁用拷贝和移动
    ThreadLocalBuffer(const ThreadLocalBuffer&) = delete;
    ThreadLocalBuffer& operator=(const ThreadLocalBuffer&) = delete;
    ThreadLocalBuffer(ThreadLocalBuffer&&) = delete;
    ThreadLocalBuffer& operator=(ThreadLocalBuffer&&) = delete;

    /**
     * @brief 前端写入接口 (Producer)
     * @return true 写入成功，false 缓冲区满被丢弃
     */
    bool push(std::string_view data) noexcept {
        if (data.size() == 0 || data.size() > CAPACITY) [[unlikely]] return false;

        // 获取当前生产进度 (Relaxed，只有当前线程会修改 tail_)
        const uint64_t current_tail = _tail.load(std::memory_order_relaxed);
        
        // 获取当前消费进度 (Acquire，确保看到消费者对 buffer_ 的读取完成)
        const uint64_t current_head = _head.load(std::memory_order_acquire);

        // 空间不足，直接丢弃
        if (CAPACITY - (current_tail - current_head) < data.size()) [[unlikely]] {
            _drop_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // 写入数据 (处理环形回绕)
        const uint64_t offset = current_tail & MASK;
        const uint64_t space_at_end = CAPACITY - offset;

        if (data.size() <= space_at_end) [[likely]] {
            // 一次性写入
            ::memcpy(_buffer + offset, data.data(), data.size());
        } else {
            // 分两段写入 (跨越了环形边界)
            ::memcpy(_buffer + offset, data.data(), space_at_end);
            ::memcpy(_buffer, data.data() + space_at_end, data.size() - space_at_end);
        }

        // 更新生产进度 (Release，保证在 tail_ 更新前 memcpy 的数据对消费者可见)
        _tail.store(current_tail + data.size(), std::memory_order_release);
        return true;
    }

    /**
     * @brief 后端消费接口
     * @param process_func 消费回调函数，签名需为 void(std::string_view data)
     * @return 消费的字节数
     */
    template <typename Func>
    size_t consume(Func&& process_func) noexcept {
        // 获取当前消费进度 (Relaxed，只有后台线程会修改 head_)
        const uint64_t current_head = _head.load(std::memory_order_relaxed);

        // 获取当前生产进度 (Acquire，确保看到生产者写入的 buffer_ 数据)
        const uint64_t current_tail = _tail.load(std::memory_order_acquire);

        if (current_head == current_tail)  {
            return 0; // 空缓冲区
        }

        const uint64_t available_data = current_tail - current_head;
        const uint64_t offset = current_head & MASK;
        const uint64_t space_at_end = CAPACITY - offset;

        // 零拷贝传递给后端处理 (处理环形回绕)
        if (available_data <= space_at_end) {
            // 数据连续
            process_func(std::string_view(_buffer + offset, available_data));
        } else {
            // 数据分两段
            process_func(std::string_view(_buffer + offset, space_at_end));
            process_func(std::string_view(_buffer, available_data - space_at_end));
        }

        // 更新消费进度 (Release，保证消费者读取完数据后，生产者才能覆盖这块内存)
        _head.store(current_head + available_data, std::memory_order_release);

        return available_data;
    }

    /**
     * @brief 获取并重置丢弃计数器 (后端调用)
     */
    uint64_t fetch_and_clear_drop_count() noexcept {
        // 99.99% 的情况下是 0，普通的 load 不会独占 Cache Line，不会引发总线风暴
        if (_drop_count.load(std::memory_order_relaxed) == 0) [[likely]] {
            return 0;
        }
        return _drop_count.exchange(0, std::memory_order_relaxed);
    }

private:
    // 内存布局
    // --- Cache Line 1: 生产者独占区 ---
    alignas(kCacheLineSize) std::atomic<uint64_t> _tail{0};

    // --- Cache Line 2: 消费者独占区 ---
    alignas(kCacheLineSize) std::atomic<uint64_t> _head{0};

    // --- Cache Line 3: 异常状态区 ---
    alignas(kCacheLineSize) std::atomic<uint64_t> _drop_count{0};
    
    // --- Cache Line 4: 实际数据区 ---
    alignas(kCacheLineSize) char _buffer[CAPACITY];
};

} // namespace KzLog