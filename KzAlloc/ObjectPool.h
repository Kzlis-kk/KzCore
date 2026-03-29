#pragma once
#include "Common.h"
#include "KzSTL/SpinMutex.h"
#include <atomic>
#include <new>
#include <cstdint>

namespace KzAlloc {

// 16 字节对齐的 Tagged Pointer，用于解决 ABA 问题
// alignas(16) 以确保底层可以使用 128-bit 原子指令 (CMPXCHG16B)
struct alignas(16) TaggedPointer {
    void* ptr;
    uint64_t tag; // 版本号，每次修改递增
};

// 专门用于分配固定大小对象（如 Span）的定长内存池
// 避免直接调用 new 导致循环依赖 malloc
template<class T>
class ObjectPool {
public:
    ObjectPool() noexcept {
        _head.store({nullptr, 0}, std::memory_order_relaxed);
    }

    ~ObjectPool() {
        // 析构时单线程执行，无需加锁
        void* cur = _currentBlock;
        while (cur) {
            void* next = NextObj(cur);
            SystemFree(cur, _blockSize >> PAGE_SHIFT);
            cur = next;
        }
    }

    [[nodiscard]] T* New() noexcept {
        T* obj = AllocateMemory();
        return new (obj) T;
    }

    void Delete(T* obj) noexcept {
        if (obj) {
            obj->~T();
            FreeMemory(obj);
        }
    }

    // 仅分配内存
    [[nodiscard]] T* AllocateMemory() noexcept {
        TaggedPointer head = _head.load(std::memory_order_acquire);
        
        while (true) {
            // 1如果链表为空，进入慢速路径去系统申请
            if (head.ptr == nullptr) [[unlikely]] {
                Refill();
                // Refill 后重新加载 head 并重试
                head = _head.load(std::memory_order_acquire);
                continue;
            }

            
            // void* next = reinterpret_cast<std::atomic<void*>*>(head.ptr)->load(std::memory_order_relaxed);
            // 读取下一个节点
            // 使用 C++20 std::atomic_ref 进行 Relaxed 读
            // 告诉编译器：这是一个可能被其他线程并发修改的值，不要做任何假设和优化！
            void* next = std::atomic_ref<void*>(NextObj(head.ptr)).load(std::memory_order_relaxed);

            // 尝试 CAS 更新头节点，tag + 1 防止 ABA
            TaggedPointer new_head{next, head.tag + 1};
            if (_head.compare_exchange_weak(head, new_head, 
                                            std::memory_order_acquire, 
                                            std::memory_order_relaxed)) {
                return static_cast<T*>(head.ptr);
            }
            // 如果 CAS 失败，head 会被自动更新为最新的值，继续循环
        }
    }

    // 无锁释放
    void FreeMemory(T* obj) noexcept {
        static_assert(sizeof(T) >= sizeof(void*), "T must be large enough to hold a pointer");
        
        TaggedPointer head = _head.load(std::memory_order_relaxed);
        TaggedPointer new_head;
        new_head.ptr = obj;

        while (true) {
            // 头插法：将当前对象的 next 指向当前的 head.ptr
            // 在插入前 obj 还是线程独占的，不需要原子写入
            NextObj(obj) = head.ptr;
            new_head.tag = head.tag + 1;

            // 尝试 CAS
            if (_head.compare_exchange_weak(head, new_head, 
                                            std::memory_order_release, 
                                            std::memory_order_relaxed)) {
                break;
            }
        }
    }

private:
    // 慢速路径：向系统申请大块内存并切分
    void Refill() noexcept {
        // 乐观分配：不加任何锁，直接向 OS 申请 128KB
        // 如果多个线程同时进入这里，它们会各自申请 128KB。这在内存池中是完全可接受的。
        void* newBlock = SystemAlloc(_blockSize >> PAGE_SHIFT);
        if (!newBlock) [[unlikely]] handleOOM();
        
        // 无锁挂载到 _currentBlock (用于析构时释放)
        void* old_block = _currentBlock.load(std::memory_order_relaxed);
        do {
            NextObj(newBlock) = old_block;
        } while (!_currentBlock.compare_exchange_weak(old_block, newBlock, 
                                                  std::memory_order_release, 
                                                  std::memory_order_relaxed));

        // 计算对齐并切分
        constexpr size_t align = alignof(T) > sizeof(void*) ? alignof(T) : sizeof(void*);
        uintptr_t raw_addr = reinterpret_cast<uintptr_t>(newBlock) + sizeof(void*);
        uintptr_t aligned_addr = (raw_addr + align - 1) & ~(align - 1);
        
        char* memory = reinterpret_cast<char*>(aligned_addr);
        size_t leftBytes = _blockSize - (aligned_addr - reinterpret_cast<uintptr_t>(newBlock));

        // 在本地构建单链表 (极大地减少 CAS 竞争)
        // 我们不一个一个 Push，而是把 128KB 切好串起来，一次性挂载到全局 _head
        void* local_head = nullptr;
        void* local_tail = nullptr;
        
        constexpr size_t objSize = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
        
        while (leftBytes >= objSize) {
            void* obj = memory;
            memory += objSize;
            leftBytes -= objSize;

            if (local_head == nullptr) {
                local_head = obj;
                local_tail = obj;
                NextObj(obj) = nullptr;
            } else {
                NextObj(obj) = local_head;
                local_head = obj;
            }
        }

        // 将本地链表一次性无锁挂载到全局 _head
        if (local_head != nullptr) {
            TaggedPointer global_head = _head.load(std::memory_order_relaxed);
            TaggedPointer new_global_head;
            new_global_head.ptr = local_head;

            do {
                // 将本地链表的尾部指向当前的全局头部
                NextObj(local_tail) = global_head.ptr;
                new_global_head.tag = global_head.tag + 1;
            } while (!_head.compare_exchange_weak(global_head, new_global_head, 
                                                  std::memory_order_release, 
                                                  std::memory_order_relaxed));
        }
    }

private:
    static constexpr size_t _blockSize = 128 * 1024; 

    // 128-bit 原子变量，存储头指针和 ABA Tag
    std::atomic<TaggedPointer> _head; 

    // 慢速路径使用指针，用于无锁追踪所有申请的大块内存
    std::atomic<void*> _currentBlock{nullptr};
};

} // namespace KzAlloc