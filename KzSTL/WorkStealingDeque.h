#pragma once

#include <atomic>
#include <cstddef>
#include <cassert>
#include <type_traits>
#include <new>

namespace KzSTL {

/**
 * @brief Chase-Lev 任务窃取双端队列
 * @tparam T 任务类型 (建议 Job<void(), 40>)
 * @tparam Capacity 队列容量，必须是 2 的幂
 */
template <typename T, size_t Capacity>
class WorkStealingDeque {
#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    static constexpr size_t kCacheLineSize = 64;
#endif
    static_assert((Capacity > 0) && ((Capacity & (Capacity - 1)) == 0), "Capacity must be a power of 2");
    static_assert(std::is_nothrow_move_constructible_v<T>, "T must be nothrow move constructible");

public:
    explicit WorkStealingDeque() noexcept {
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < Capacity; ++i) {
            _buffer[i].is_extracted.store(true, std::memory_order_relaxed);
        }
    }

    ~WorkStealingDeque() {
        size_t head = _head.load(std::memory_order_relaxed);
        size_t tail = _tail.load(std::memory_order_relaxed);
        for (size_t i = head; i < tail; ++i) {
            Slot& slot = _buffer[i & Mask];
            if (!slot.is_extracted.load(std::memory_order_relaxed)) {
                T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
                ptr->~T();
            }
        }
    }

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

    /**
     * @brief 本地线程推入任务 (Zero-CAS)
     * @return true 成功, false 队列已满
     */
    template <typename... Args>
    bool push(Args&&... args) noexcept {
        static_assert(std::is_constructible_v<T, Args...>, "T cannot be constructed from these arguments");

        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t head = _head.load(std::memory_order_acquire);
        
        if (tail - head >= Capacity) [[unlikely]] {
            return false; // 队列满
        }

        Slot& slot = _buffer[tail & Mask];
        
        // 防御 Wrap-around Bug
        // 等待可能被挂起的 Stealer 完成数据提取。这在实际中极少发生。
        while (!slot.is_extracted.load(std::memory_order_acquire)) [[unlikely]] {
            #if defined(_MSC_VER)
            _mm_pause();
            #elif defined(__GNUC__) || defined(__clang__)
            __builtin_ia32_pause();
            #endif
        }

        // 在生肉上构造对象
        T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
        new (ptr) T(std::forward<Args>(args)...);
        
        // 标记为未提取
        slot.is_extracted.store(false, std::memory_order_relaxed);
        
        // 更新 _tail, 保证数据写入对 Stealer 可见
        _tail.store(tail + 1, std::memory_order_release);
        
        return true;
    }

    /**
    * @brief 批量推入任务 (极速路径)
    * @param items 任务的连续内存视图
    * @return 实际成功推入的数量
    */
    size_t pushBulk(std::span<T> items) noexcept {
        if (items.empty()) [[unlikely]] return 0;

        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t head = _head.load(std::memory_order_acquire);
    
        // 计算实际能推入的数量 (不能超过队列剩余容量)
        size_t available = Capacity - (tail - head);
        size_t count = std::min(items.size(), available);
        if (count == 0) return 0;

        for (size_t i = 0; i < count; ++i) {
            Slot& slot = _buffer[(tail + i) & Mask];
        
            // 防御 Wrap-around
            while (!slot.is_extracted.load(std::memory_order_acquire)) [[unlikely]] {
                #if defined(_MSC_VER)
                _mm_pause();
                #elif defined(__GNUC__) || defined(__clang__)
                __builtin_ia32_pause();
                #endif
            }

            T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
            new (ptr) T(std::move(items[i]));
            slot.is_extracted.store(false, std::memory_order_relaxed);
        }
    
        // 只执行一次 bottom 的更新
        _tail.store(tail + count, std::memory_order_release);
    
        return count;
    }

    /**
     * @brief 本地线程弹出任务 (LIFO, Zero-CAS)
     * @return true 成功, false 队列为空
     */
    bool pop(T& result) noexcept {
        size_t tail = _tail.load(std::memory_order_relaxed);
        if (tail == 0) return false; // 判空
        
        tail = tail - 1;
        _tail.store(tail, std::memory_order_relaxed);

        // 全屏障，防止 _tail 的 store 和 _head 的 load 发生指令重排
        std::atomic_thread_fence(std::memory_order_seq_cst);

        size_t head = _head.load(std::memory_order_relaxed);

        if (head <= tail) {
            // 队列非空
            Slot& slot = _buffer[tail & Mask];
            T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));

            if (head == tail) {
                // 队列只剩最后一个元素，可能与 Stealer 发生竞争
                if (!_head.compare_exchange_strong(head, head + 1, 
                                                  std::memory_order_seq_cst, 
                                                  std::memory_order_relaxed)) {
                    // 竞争失败，Stealer 抢走了任务
                    _tail.store(tail + 1, std::memory_order_relaxed); // 恢复 _tail
                    return false;
                }
                // 竞争成功，我们拿到了最后一个任务
                result = std::move(*ptr);
                ptr->~T();
                slot.is_extracted.store(true, std::memory_order_release);
                
                _tail.store(tail + 1, std::memory_order_relaxed); // 队列彻底空了
                return true;
            }

            // 正常情况：队列元素 > 1，绝对安全，直接拿走
            result = std::move(*ptr);
            ptr->~T();
            slot.is_extracted.store(true, std::memory_order_release);
            return true;
        } else {
            // 队列为空 (在我们减 _tail 之前，Stealer 已经把队列偷空了)
            _tail.store(tail + 1, std::memory_order_relaxed); // 恢复 _tail
            return false;
        }
    }

    /**
     * @brief 其他线程窃取任务 (FIFO, 需要 CAS)
     * @return true 成功, false 队列为空或竞争失败
     */
    bool steal(T& result) noexcept {
        size_t head = _head.load(std::memory_order_acquire);
        
        // 全屏障，确保读取 top 和 bottom 的顺序
        std::atomic_thread_fence(std::memory_order_seq_cst);
        
        size_t tail = _tail.load(std::memory_order_acquire);

        if (head < tail) {
            // 尝试抢占队头
            if (_head.compare_exchange_strong(head, head + 1, 
                                             std::memory_order_seq_cst, 
                                             std::memory_order_relaxed)) {
                // 抢占成功！现在这个 Slot 归我们了
                Slot& slot = _buffer[head & Mask];
                T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
                
                result = std::move(*ptr);
                ptr->~T();
                
                // 释放 Slot，允许 Owner 未来覆盖写入
                slot.is_extracted.store(true, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    size_t sizeApprox() const noexcept {
        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t head = _head.load(std::memory_order_relaxed);
        return tail >= head ? tail - head : 0;
    }

private:
    static constexpr size_t Mask = Capacity - 1;

    struct Slot {
        // 56 字节的数据
        alignas(T) std::byte data[sizeof(T)];
        // 1 字节的状态标志，用于防御 Wrap-around Bug
        std::atomic<bool> is_extracted;
        // 编译器会自动填充 7 字节，使得 Slot 严格等于 64 字节
    };

    static_assert(sizeof(Slot) % kCacheLineSize == 0, "Slot must perfectly fit in one Cache Line");

    alignas(kCacheLineSize) std::atomic<size_t> _head;
    alignas(kCacheLineSize) std::atomic<size_t> _tail;
    alignas(kCacheLineSize) Slot _buffer[Capacity];
};

} // namespace KzSTL