#pragma once

#include <atomic>
#include <cstddef>
#include <cassert>
#include <type_traits>
#include <new>
#include <span>

namespace KzSTL {

/**
 * @brief 高性能多生产者单消费者 (MPSC) 无锁队列
 * @tparam T 元素类型
 * @tparam Capacity 队列容量，必须是 2 的幂
 */
template <typename T, size_t Capacity>
class ArrayMpscQueue {
#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    static constexpr size_t kCacheLineSize = 64;
#endif
    static_assert((Capacity > 0) && ((Capacity & (Capacity - 1)) == 0), "Capacity must be a power of 2");
    static_assert(std::is_nothrow_move_constructible_v<T>, "T must be nothrow move constructible");

public:
    explicit ArrayMpscQueue() noexcept {
        // _head 由消费者独占，初始化时 relaxed 即可
        _head.store(0, std::memory_order_relaxed);
        // _tail 是生产者争用点，初始化时 relaxed 即可
        _tail.store(0, std::memory_order_relaxed);
        // 初始化所有槽位为“已提取”状态
        for (size_t i = 0; i < Capacity; ++i) {
            _buffer[i].is_written.store(false, std::memory_order_relaxed);
        }
    }

    ~ArrayMpscQueue() {
        size_t head = _head.load(std::memory_order_relaxed);
        size_t tail = _tail.load(std::memory_order_relaxed);
        for (size_t i = head; i < tail; ++i) {
            Slot& slot = _buffer[i & Mask];
            if (slot.is_written.load(std::memory_order_relaxed)) {
                T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
                ptr->~T();
            }
        }
    }

    ArrayMpscQueue(const ArrayMpscQueue&) = delete;
    ArrayMpscQueue& operator=(const ArrayMpscQueue&) = delete;

    /**
     * @brief 多线程并发推入任务 (生产者)
     * @return true 成功, false 队列已满
     */
    template <typename... Args>
    bool push(Args&&... args) noexcept {
        static_assert(std::is_constructible_v<T, Args...>, "T cannot be constructed from these arguments");

        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t head = _head.load(std::memory_order_acquire);

        // CAS 循环：安全地预占槽位，避免 fetch_sub 带来的多线程回滚灾难
        while (true) {
            size_t size = tail - head;
            if (size >= Capacity) {
                // 重新加载 head 以确认是否真的满了
                head = _head.load(std::memory_order_acquire);
                size = tail - head;
                if (size >= Capacity) {
                    return false; // 队列已满
                }
            }
            // 尝试推进 tail
            if (_tail.compare_exchange_weak(tail, tail + 1, std::memory_order_relaxed)) {
                break;
            }
        }

        // 等待消费者处理完这个可能被复用的槽位 (防御 Wrap-around)
        Slot& slot = _buffer[tail & Mask];
        while (slot.is_written.load(std::memory_order_acquire)) [[unlikely]] {
            #if defined(_MSC_VER)
            _mm_pause();
            #elif defined(__GNUC__) || defined(__clang__)
            __builtin_ia32_pause();
            #endif
        }

        // 构造对象并发布
        T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
        new (ptr) T(std::forward<Args>(args)...);
        slot.is_written.store(true, std::memory_order_release);
        
        return true;
    }

    /**
     * @brief 多线程并发批量推入任务 (生产者)
     * @param items 要推入的任务视图
     * @return 实际成功推入的数量
     */
    size_t pushBulk(std::span<T> items) noexcept {
        const size_t count = items.size();
        if (count == 0) [[unlikely]] return 0;

        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t head = _head.load(std::memory_order_acquire);
        size_t actual_count = 0;

        // CAS 循环：安全地批量预占槽位
        while (true) {
            size_t size = tail - head;
            size_t available = size >= Capacity ? 0 : Capacity - size;
            
            if (available < count) {
                head = _head.load(std::memory_order_acquire);
                size = tail - head;
                available = size >= Capacity ? 0 : Capacity - size;
                if (available == 0) {
                    return 0; // 队列已满
                }
            }
            
            actual_count = std::min(count, available);
            if (_tail.compare_exchange_weak(tail, tail + actual_count, std::memory_order_relaxed)) {
                break;
            }
        }

        // 批量构造对象并发布
        for (size_t i = 0; i < actual_count; ++i) {
            Slot& slot = _buffer[(tail + i) & Mask];
            
            while (slot.is_written.load(std::memory_order_acquire)) [[unlikely]] {
                #if defined(_MSC_VER)
                _mm_pause();
                #elif defined(__GNUC__) || defined(__clang__)
                __builtin_ia32_pause();
                #endif
            }

            T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
            new (ptr) T(std::move(items[i]));
            
            slot.is_written.store(true, std::memory_order_release);
        }
        
        return actual_count;
    }

    /**
     * @brief 本地线程弹出任务 (单消费者)
     * @return true 成功, false 队列为空
     */
    bool pop(T& result) noexcept {
        // 1. 读取 head，消费者独占，无需原子操作
        size_t head = _head.load(std::memory_order_relaxed);
        Slot& slot = _buffer[head & Mask];

        // 2. 检查槽位是否有数据
        // 使用 acquire 内存序，确保能看到生产者线程的 release store。
        if (!slot.is_written.load(std::memory_order_acquire)) {
            return false; // 队列为空
        }

        // 3. 提取数据
        T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
        result = std::move(*ptr);
        ptr->~T();

        // 4. 标记为“未写入”（即“已提取”），允许生产者复用
        // 使用 release 内存序，确保其他生产者能看到此槽位已变为空闲。
        slot.is_written.store(false, std::memory_order_release);

        // 5. 前进 head 指针，发布空闲槽位
        // 使用 release 内存序，确保对 is_written 的写入对生产者可见。
        _head.store(head + 1, std::memory_order_release);
        
        return true;
    }

    /**
     * @brief 本地线程批量弹出任务 (单消费者)
     * @param items 用于接收任务的视图
     * @return 实际成功弹出的数量
     */
    size_t popBulk(std::span<T> items) noexcept {
        const size_t count = items.size();
        if (count == 0) [[unlikely]] return 0;

        const size_t head = _head.load(std::memory_order_relaxed);
        size_t i = 0;

        // 消费者无需读取 _tail，直接根据 is_written 标志位判断即可。
        // 这样避免了被“已预占但尚未写入”的生产者线程阻塞，保证了 Wait-Free 特性。
        for (; i < count; ++i) {
            Slot& slot = _buffer[(head + i) & Mask];

            if (!slot.is_written.load(std::memory_order_acquire)) {
                break; // 遇到未准备好的槽位，直接中断返回
            }

            T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
            items[i] = std::move(*ptr);
            ptr->~T();
            
            slot.is_written.store(false, std::memory_order_release);
        }

        if (i > 0) {
            // 批量更新 head，减少对 head 缓存行的写操作
            _head.store(head + i, std::memory_order_release);
        }

        return i;
    }

    /**
     * @brief 获取队列大致大小 (非精确值)
     */
    size_t sizeApprox() const noexcept {
        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t head = _head.load(std::memory_order_relaxed);
        return tail >= head ? tail - head : 0;
    }

private:
    static constexpr size_t Mask = Capacity - 1;

    struct Slot {
        alignas(T) std::byte data[sizeof(T)];
        // 状态标志，true 表示已写入数据，false 表示空闲
        std::atomic<bool> is_written;
    };

    static_assert(sizeof(Slot) % kCacheLineSize == 0, "Slot must perfectly fit in one or more Cache Lines");

    // _head 由单消费者独占访问，但需要被生产者读取以判断队列是否已满
    alignas(kCacheLineSize) std::atomic<size_t> _head;
    
    // _tail 由多生产者并发访问，是主要的争用点
    alignas(kCacheLineSize) std::atomic<size_t> _tail;

    // 循环缓冲区
    alignas(kCacheLineSize) Slot _buffer[Capacity];
};

} // namespace KzSTL