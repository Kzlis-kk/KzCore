#pragma once

#include <atomic>
#include <cstddef>
#include <cassert>
#include <type_traits>
#include <new>
#include <span>

namespace KzSTL {

template <typename T, size_t Capacity>
class ArrayMpmcQueue {
#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    static constexpr size_t kCacheLineSize = 64;
#endif
    static_assert((Capacity > 0) && ((Capacity & (Capacity - 1)) == 0), "Capacity must be a power of 2");
    static_assert(std::is_nothrow_move_constructible_v<T>, "T must be nothrow move constructible");
    static_assert(std::is_nothrow_move_assignable_v<T>, "T must be nothrow move assignable");
    static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");

public:
    explicit ArrayMpmcQueue() noexcept {
        for (int i = 0; i < Capacity; ++i) {
            _buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
    }

    ~ArrayMpmcQueue() {
        // 队列析构时，必须清理尚未出队的元素，防止内存泄漏
        // 此时假设已经没有并发读写
        size_t head = _head.load(std::memory_order_relaxed);
        size_t tail = _tail.load(std::memory_order_relaxed);
        for (size_t i = head; i != tail; ++i) {
            Slot& slot = _buffer[i & Mask];
            // 只有 sequence == i + 1，才说明入队操作已经彻底完成
            if (slot.sequence.load(std::memory_order_acquire) == i + 1) {
                T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
                ptr->~T();
            }
        }
    }

    ArrayMpmcQueue(const ArrayMpmcQueue&) = delete;
    ArrayMpmcQueue& operator=(const ArrayMpmcQueue&) = delete;

    size_t enqueueBulk(std::span<T> items) noexcept {
        if (items.empty()) [[unlikely]] return 0;

        size_t tail = _tail.load(std::memory_order_relaxed);

        while (true) {
            size_t freeNum = 0;
            // 防御：单次批量入队数量绝对不能超过 Capacity
            size_t maxTry = std::min<size_t>(items.size(), Capacity);
            for (size_t i = 0; i < maxTry; ++i) {
                size_t curPos = tail + i;
                Slot& slot = _buffer[curPos & Mask];
                size_t seq = slot.sequence.load(std::memory_order_acquire);
                
                if (seq == curPos) {
                    freeNum++;
                }
                else {
                    break;
                }
            }

            if (freeNum == 0) {
                Slot& slot = _buffer[tail & Mask];
                size_t seq = slot.sequence.load(std::memory_order_acquire);
                if (seq < tail) {
                    return 0;
                }
                tail = _tail.load(std::memory_order_relaxed);
                continue;
                }

            if (_tail.compare_exchange_weak(tail, tail + freeNum, std::memory_order_relaxed)) {
                for (size_t i = 0; i < freeNum; ++i) {
                    size_t curPos = tail + i;
                    Slot& slot = _buffer[curPos & Mask];
                    // Placement New 在生肉上构造对象
                    T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
                    new (ptr) T(std::move(items[i]));

                    slot.sequence.store(curPos + 1, std::memory_order_release);
                }
                return freeNum;
            }

        }
    }

    template <typename... Args>
    bool enqueue(Args&&... args) noexcept {
        static_assert(std::is_constructible_v<T, Args...>, "T cannot be constructed from these arguments");

        size_t tail = _tail.load(std::memory_order_relaxed);
        while (true) {
            Slot& slot = _buffer[tail & Mask];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            if (seq == tail) {
                if (_tail.compare_exchange_weak(tail, tail + 1, std::memory_order_relaxed)) {
                    T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
                    new (ptr) T(std::forward<Args>(args)...);
                    slot.sequence.store(tail + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (seq < tail) {
                return false;
            }
            else {
                tail = _tail.load(std::memory_order_relaxed);
            }
        }
    }

    template <typename OutputIterator>
    size_t dequeueBulk(OutputIterator it , size_t maxBatch) noexcept {
        size_t head = _head.load(std::memory_order_relaxed);

        while (true) {
            size_t count = 0;
            for (size_t i = 0; i < maxBatch; ++i) {
                size_t curPos = head + i;
                Slot& slot = _buffer[curPos & Mask];
                size_t seq = slot.sequence.load(std::memory_order_acquire);
                if (seq == curPos + 1) {
                    count++;
                }
                else {
                    break;
                }
            }

            if (count == 0) {
                Slot& slot = _buffer[head & Mask];
                size_t seq = slot.sequence.load(std::memory_order_acquire);
                if (seq < head + 1) {
                    return 0;
                }
                head = _head.load(std::memory_order_relaxed);
                continue;
            }

            if (_head.compare_exchange_weak(head, head + count, std::memory_order_relaxed)) {
                for (size_t i = 0; i < count; ++i) {
                    size_t curPos = head + i;
                    Slot& slot = _buffer[curPos & Mask];

                    // 移出数据并显式析构
                    T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
                    *it++ = std::move(*ptr);
                    ptr->~T(); 

                    slot.sequence.store(curPos + Capacity, std::memory_order_release);
                }
                return count;
            }

        }
    }

    bool dequeue(T& result) noexcept {
        size_t head = _head.load(std::memory_order_relaxed);

        while (true) {
            Slot& slot = _buffer[head & Mask];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            if (seq == head + 1) {
                if (_head.compare_exchange_weak(head, head + 1, std::memory_order_relaxed)) {
                    T* ptr = std::launder(reinterpret_cast<T*>(&slot.data));
                    result = std::move(*ptr);
                    ptr->~T();
                    
                    slot.sequence.store(head + Capacity, std::memory_order_release);
                    return true;
                }
            }
            else if (seq < head + 1) {
                return false;
            }
            else {
                head = _head.load(std::memory_order_relaxed);
            }
        }
    }

    size_t sizeApprox() const noexcept {
        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t head = _head.load(std::memory_order_relaxed);

        return tail >= head ? tail - head : 0;
    }

private:
    static constexpr size_t Mask = Capacity - 1;

    struct Slot {
        std::atomic<size_t> sequence;
        // 使用 std::byte 数组占位，并强制对齐到 T 的要求
        alignas(T) std::byte data[sizeof(T)];
    };

    // 保证数组起始地址对齐，且 _head, _tail 互相隔离
    alignas(kCacheLineSize) std::atomic<size_t> _head;
    alignas(kCacheLineSize) std::atomic<size_t> _tail;
    alignas(kCacheLineSize) Slot _buffer[Capacity];

    static_assert(sizeof(Slot) == kCacheLineSize, "Slot must perfectly fit in one Cache Line");
};

} // namespace KzSTL