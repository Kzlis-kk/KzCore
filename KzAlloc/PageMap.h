#pragma once
#include "Common.h"
#include "Span.h"
#include <cstring>
#include <mutex>
#include <new>
#include <array>
#include <algorithm>

namespace KzAlloc {

// Radix Tree 配置 (根据平台自动调整层级)
class PageMap {
private:
    // 64位系统: 3 层基数树
    // 有效虚拟地址 48位 - 页偏移 13位 = 35位有效 PageID
    // 划分: Root(12bit) -> Internal(12bit) -> Leaf(11bit)
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    static constexpr int RADIX_TREE_LEVELS = 3;
    static constexpr int BITS_ROOT = 12;
    static constexpr int BITS_INTERNAL = 12;
    static constexpr int BITS_LEAF = 11;
    static constexpr int BITS_INTERNAL_AND_LEAF = BITS_LEAF + BITS_INTERNAL;

    // 32位系统: 2 层基数树
    // 有效虚拟地址 32位 - 页偏移 13位 = 19位有效 PageID
    // 划分: Root(5bit) -> Leaf(14bit)
#else
    static constexpr int RADIX_TREE_LEVELS = 2;
    static constexpr int BITS_ROOT = 5;
    static constexpr int BITS_LEAF = 14;
    static constexpr int BITS_INTERNAL = 0;
#endif

    // 计算各层数组长度
    static constexpr size_t LEN_ROOT = 1 << BITS_ROOT;
    static constexpr size_t LEN_INTERNAL = 1 << BITS_INTERNAL;
    static constexpr size_t LEN_INTERNAL_ROUND_UP_NUM = LEN_INTERNAL - 1;
    static constexpr size_t LEN_LEAF = 1 << BITS_LEAF;
    static constexpr size_t LEN_LEAF_ROUND_UP_NUM = LEN_LEAF - 1;

private:
    // Leaf Node: 最底层，使用 atomic 包装 Span*，保证并发读写的绝对安全
    struct PageMapLeaf {
        std::array<std::atomic<Span*>, LEN_LEAF> values;
    };

    // Internal Node: 中间层，使用 atomic 包装 Leaf*
    // 仅在 3 层模式下有效
    struct PageMapInternal {
        std::array<std::atomic<PageMapLeaf*>, LEN_INTERNAL> leafs;
    };

public:
    static PageMap* GetInstance() {
    alignas(PageMap) static char _buffer[sizeof(PageMap)];
    static PageMap* _instance = nullptr;
    
    // C++ 要求要有返回值
    static const bool _inited = [&]() {
        _instance = new (_buffer) PageMap();
        return true;
    }();
    
    static_cast<void>(_inited);
    // 返回解引用后的对象
    return _instance;
    }

    // ---------------------------------------------------------------------
    // 查映射 (Read): O(1) 无锁
    // ---------------------------------------------------------------------
    [[nodiscard]] inline Span* get(PAGE_ID id) const noexcept {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
        // [Layer 1]
        size_t iRoot = id >> BITS_INTERNAL_AND_LEAF;
        if (iRoot >= LEN_ROOT) [[unlikely]] return nullptr;
        // 因为指针赋值是原子的，所以直接读即可
        // 如果读到 nullptr 说明还没分配，直接返回
        auto rootNode = _root[iRoot].load(std::memory_order_acquire);
        if (rootNode == nullptr) [[unlikely]] return nullptr;

        // [Layer 2]
        size_t iInternal = (id >> BITS_LEAF) & LEN_INTERNAL_ROUND_UP_NUM;
        auto leafNode = rootNode->leafs[iInternal].load(std::memory_order_acquire);
        if (leafNode == nullptr) [[unlikely]] return nullptr;

        // [Layer 3]
        size_t iLeaf = id & LEN_LEAF_ROUND_UP_NUM;
        return leafNode->values[iLeaf].load(std::memory_order_relaxed);
#else
        // 32-bit Logic
        size_t iRoot = id >> BITS_LEAF;
        if (iRoot >= LEN_ROOT) return nullptr;
        
        auto leafNode = _root[iRoot].load(std::memory_order_acquire);;
        if (leafNode == nullptr) return nullptr;

        size_t iLeaf = id & LEN_LEAF_ROUND_UP_NUM;
        return leafNode->values[iLeaf].load(std::memory_order_relaxed);
#endif
    }

    // ---------------------------------------------------------------------
    // 设映射 (Write): 线程安全, 返回 bool，支持 OOM 回滚
    // set 通常在 PageCache 有锁的情况下调用，但为了 RadixTree 自身的稳健性，
    // 我们在节点扩容(Ensure)时使用了内部锁
    // ---------------------------------------------------------------------
    bool set(PAGE_ID id, Span* span) noexcept {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
        // 1. 确保 Root -> Internal 存在
        size_t iRoot = id >> BITS_INTERNAL_AND_LEAF;
        if (!EnsureRoot(iRoot)) [[unlikely]] return false; // OOM
        
        // 2. 确保 Internal -> Leaf 存在
        size_t iInternal = (id >> BITS_LEAF) & LEN_INTERNAL_ROUND_UP_NUM;
        if (!EnsureInternal(iRoot, iInternal)) [[unlikely]] return false; // OOM

        // 3. 写入最终值
        size_t iLeaf = id & LEN_LEAF_ROUND_UP_NUM;
        // 此时 Ensure 已经保证了节点存在，直接 relaxed 读取即可
        auto* rootNode = _root[iRoot].load(std::memory_order_relaxed);
        auto* leafNode = rootNode->leafs[iInternal].load(std::memory_order_relaxed);
        // 按字长(8字节)对齐时，直接汇编优化为单 mov 操作，而不存在 64 位撕裂导致前 32 位和后 32 位分开 mov
        leafNode->values[iLeaf].store(span, std::memory_order_relaxed);
        return true;
#else
        size_t iRoot = id >> BITS_LEAF;
        if (!EnsureRoot(iRoot)) [[unlikely]] return false;
        size_t iLeaf = id & LEN_LEAF_ROUND_UP_NUM;
        auto* leafNode = _root[iRoot].load(std::memory_order_relaxed);
        leafNode->values[iLeaf].store(span, std::memory_order_relaxed);
        return true;
#endif
    }

private:
    explicit PageMap() noexcept = default; 

    PageMap(const PageMap&) = delete;
    PageMap& operator=(const PageMap&) = delete;

    // 辅助函数：申请节点内存 (使用 SystemAlloc 绕过 malloc)
    // 自动计算需要申请的页数
    // 这里不用接口 allocate() 主要是因为对象较大
    template<typename NodeType>
    static NodeType* AllocNode() noexcept {
        constexpr size_t size = sizeof(NodeType);
        constexpr size_t kpages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
        
        void* ptr = SystemAlloc(kpages);
        if (ptr == nullptr) [[unlikely]] return nullptr;
        // SystemAlloc (mmap/VirtualAlloc) 保证返回的内存是清零的
        // 所以我们不需要手动 memset 0
        // 使用 placement new 调用构造函数，确保 atomic 变量被正确初始化
        return new (ptr) NodeType();
    }

    // 确保第一层节点存在
    [[nodiscard]] bool EnsureRoot(size_t index) noexcept {
        if (index >= LEN_ROOT) [[unlikely]] return false;
        
        // Double-Checked Locking (双重检查锁)
        // 绝大多数情况 _root[index] 已经存在，无锁检查能极大提升性能
        // Acquire 语义：确保如果读到非空指针，其指向的内存一定是初始化好的
        auto* node = _root[index].load(std::memory_order_acquire);
        if (node == nullptr) [[unlikely]] {
            std::lock_guard<std::mutex> lock(_growMtx);
            // 锁内 Relaxed 读即可
            node = _root[index].load(std::memory_order_relaxed);
            if (node == nullptr) {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
                node = AllocNode<PageMapInternal>();
                if (node == nullptr) [[unlikely]] return false; // OOM
#else
                node = AllocNode<PageMapLeaf>();
                if (node == nullptr) [[unlikely]] return false; // OOM
#endif
                // Release 语义：确保 node 的内存初始化指令，绝对不会重排到 store 之后
                _root[index].store(node, std::memory_order_release);
            }
        }
        return true;
    }

#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    // 确保第二层节点存在
    [[nodiscard]] bool EnsureInternal(size_t iRoot, size_t iInternal) noexcept {
        auto* rootNode = _root[iRoot].load(std::memory_order_relaxed); // 外层已保证存在
        auto* leafNode = rootNode->leafs[iInternal].load(std::memory_order_acquire);

        if (leafNode == nullptr) [[unlikely]] {
            std::lock_guard<std::mutex> lock(_growMtx);
            leafNode = rootNode->leafs[iInternal].load(std::memory_order_relaxed);
            if (leafNode == nullptr) {
                leafNode = AllocNode<PageMapLeaf>();
                if (leafNode == nullptr) return false; // OOM
                rootNode->leafs[iInternal].store(leafNode, std::memory_order_release);
            }
        }
        return true;
    }
#endif

private:
    // 必须使用 atomic 包装指针
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    std::atomic<PageMapInternal*> _root[LEN_ROOT]{};
#else
    std::atomic<PageMapLeaf*> _root[LEN_ROOT]{};
#endif

    // 生长锁：只保护节点分配（AllocNode），不保护 Leaf 值的读写
    std::mutex _growMtx; 
};

} // namespace KzAlloc