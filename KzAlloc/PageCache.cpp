#include "PageCache.h"
#include <iostream>
#include <cassert>
#include <new>

namespace KzAlloc {

// PageHeap 实现 (全局路由与自举)
PageHeap::PageHeap() noexcept {
    // 探测硬件核心数
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 8; // 兜底

    // 设定目标分片数
    // 针对高核心数机器(>=32)使用 4倍冗余，普通机器 2倍
    size_t target_shards = cores >= 32 ? cores * 4 : cores * 2;

    // 向上取整为 2 的幂 (Next Power of 2) 以便使用位掩码路由
    _shardCount = 1;
    while (_shardCount < target_shards) {
        _shardCount <<= 1;
    }
    
    // 计算路由掩码
    _shardMask = _shardCount - 1;

    // 向 OS 申请裸内存来存放分片数组
    size_t arrayBytes = sizeof(PageCacheShard) * _shardCount;
    size_t kpages = (arrayBytes + PAGE_SIZE - 1) >> PAGE_SHIFT;

    _raw_mem = SystemAlloc(kpages);
}

PageHeap::~PageHeap() {
    if (_raw_mem) [[likely]] {
        // 显式调用析构函数
        PageCacheShard* shards = static_cast<PageCacheShard*>(_raw_mem);
        for (size_t i = 0; i < _shardCount; ++i) {
            if (_init_state[i].load(std::memory_order_acquire) == 2) {
                shards[i].~PageCacheShard();
            }
        }
        
        // 归还物理内存
        size_t arrayBytes = sizeof(PageCacheShard) * _shardCount;
        size_t kpages = (arrayBytes + PAGE_ROUND_UP_NUM) >> PAGE_SHIFT;
        SystemFree(_raw_mem, kpages);
        
        _raw_mem = nullptr;
    }
}

size_t PageHeap::GetShardIndex() noexcept {
    static std::atomic<size_t> thread_counter{0};
    // thread_local 保证每个线程只获取一次
    static thread_local size_t index = thread_counter.fetch_add(1, std::memory_order_relaxed) & _shardMask;
    return index;
}

Span* PageHeap::NewSpan(size_t k) noexcept {
    size_t idx = GetShardIndex();
    PageCacheShard* shard = GetShard(idx); // 获取或初始化
    
    // 路由到指定分片
    Span* span = shard->NewSpan(k);
    
    // 标记出生地
    // 必须在这里标记，因为 Shard 内部不知道自己的 Index
    if (span) [[likely]] {
        span->_shardId = static_cast<uint8_t>(idx);
    }
    
    return span;
}

PageCacheShard* PageHeap::GetShard(size_t idx) noexcept {
    PageCacheShard* target_addr = static_cast<PageCacheShard*>(_raw_mem) + idx;

    // 快速路径：已初始化
    if (_init_state[idx].load(std::memory_order_acquire) == 2) [[likely]] {
        return target_addr;
    }

    // 慢速路径：抢占初始化权
    int expected = 0;
    if (_init_state[idx].compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
        // 计算阈值
        size_t totalRam = GetSystemPhysicalMemory();
        size_t maxCacheBytes = totalRam / 4; 
        size_t hardLimit = 4ULL * 1024 * 1024 * 1024; 
        if (maxCacheBytes > hardLimit) maxCacheBytes = hardLimit;
        size_t totalThresholdPages = maxCacheBytes >> PAGE_SHIFT;
        size_t shardThreshold = totalThresholdPages / _shardCount;
        if (shardThreshold < 4096) shardThreshold = 4096;

        // NUMA First Touch
        // 当前线程执行 placement new，写入内存，OS 会将该物理页分配到当前线程的 NUMA 节点
        new (target_addr) PageCacheShard(shardThreshold, static_cast<uint8_t>(idx));

        // 标记完成
        _init_state[idx].store(2, std::memory_order_release);
        return target_addr;
    } else {
        // 没抢到，说明别的线程正在初始化，自旋等待其完成
        while (_init_state[idx].load(std::memory_order_acquire) != 2) {
#if defined(_MSC_VER)
            _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
            __builtin_ia32_pause();
#endif
        }
        return target_addr;
    }
}

void PageHeap::ReleaseSpan(Span* span) noexcept {
    if (!span) [[unlikely]] return;

    // 读取出生地，归还给原分片
    // 实现了 Arena Isolation，无需担心跨分片死锁
    size_t idx = span->_shardId;
    
    // 简单的越界检查 (理论上不可能触发)
    assert(idx < _shardCount); 
    
    PageCacheShard* shard = GetShard(idx);
    shard->ReleaseSpan(span);
}

// =========================================================================
// PageCacheShard 实现 (核心逻辑)
// =========================================================================


Span* PageCacheShard::NewSpan(size_t k) noexcept {
    std::lock_guard<KzSTL::SpinMutex> lock(_mutex);

    while (true) {
    // Phase 1: 尝试从 Hot Cache (热数据) 获取
    if (k < NPAGES) [[likely]] {
        // 1.1 小对象 Hot Array (Exact Match)
        if (!_spanLists[k].Empty()) {
            return AllocFromHotList(_spanLists[k], k);
        }
        // 1.2 小对象 Hot Array (Split Match)
        for (size_t i = k + 1; i < NPAGES; ++i) {
            if (!_spanLists[i].Empty()) {
                return AllocFromHotList(_spanLists[i], k);
            }
        }
    }
    // 1.3 大对象 Hot Map
    auto it = _largeSpanLists.lower_bound(k);
    if (it != _largeSpanLists.end()) {
        // 合并操作可能会把 Map 清空
        if (it->second.Empty()) {
            it = _largeSpanLists.erase(it);
            continue;
        }
        Span* ret = AllocFromMap(_largeSpanLists, it, k, false);
        if (ret == nullptr) continue;
        return ret;
    }

    // Phase 2: 尝试从 Cold Cache (冷数据) 获取
    // 既然热的没货，与其 SystemAlloc，不如复用冷的 (省去 alloc_pages 开销)
    if (k < NPAGES) {
        // 2.1 小对象 Cold Array (Exact Match)
        if (!_releasedSpanLists[k].Empty()) {
            return AllocFromColdList(_releasedSpanLists[k], k);
        }
        // 2.2 小对象 Cold Array (Split Match)
        for (size_t i = k + 1; i < NPAGES; ++i) {
            if (!_releasedSpanLists[i].Empty()) {
                return AllocFromColdList(_releasedSpanLists[i], k);
            }
        }
    }
    
    
    // 2.3 大对象 Cold Map (覆盖了 k >= NPAGES 的情况，
    // 也覆盖了 k < NPAGES 但 Cold Array 没货只能切分大块的情况)
    if (!_releasedLargeSpanLists.empty()) {
        auto it = _releasedLargeSpanLists.lower_bound(k);
        if (it != _releasedLargeSpanLists.end()) {
            if (it->second.Empty()) {
                // 合并操作可能会把 Map 清空
                it = _releasedLargeSpanLists.erase(it);
                continue;
            }
            Span* ret = AllocFromMap(_releasedLargeSpanLists, it, k, true);
            if (ret == nullptr) continue;
            return ret;
        }
    }
    
    // Phase 3: SystemAlloc (兜底)
    // 如果是大对象，直接申请 k
    _mutex.unlock();
    size_t alloc_pages = (k >= NPAGES) ? k : (NPAGES - 1);
    void* ptr = SystemAlloc(alloc_pages); 
    _mutex.lock();
    if (!ptr) [[unlikely]] return nullptr; // 内存耗尽

    Span* span = _spanPool.New();
    if (!span) [[unlikely]] {
        SystemFree(ptr, alloc_pages);
        return nullptr; // 对象池耗尽
    }

    span->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
    span->_n = alloc_pages;
    span->_isCold = false;
    span->_shardId = _shardId;
        
    // 事务性建立映射
    bool success = true;
    size_t mapped_count = 0;
        
    if (k >= NPAGES) {
        // 大对象：映射所有页
        for (; mapped_count < alloc_pages; ++mapped_count) {
            if (!PageMap::GetInstance()->set(span->_pageId + mapped_count, span)) {
                success = false; break;
            }
        }
    } else {
        // 小对象批发：只映射首尾
        if (!PageMap::GetInstance()->set(span->_pageId, span)) success = false;
        else {
            mapped_count = 1;
            if (!PageMap::GetInstance()->set(span->_pageId + span->_n - 1, span)) success = false;
            else mapped_count = 2;
        }
    }

    // OOM 回滚
    if (!success) [[unlikely]] {
        if (k >= NPAGES) {
            for (size_t j = 0; j < mapped_count; ++j) {
                PageMap::GetInstance()->set(span->_pageId + j, nullptr);
            }
        } else {
            if (mapped_count >= 1) PageMap::GetInstance()->set(span->_pageId, nullptr);
            // mapped_count == 2 的情况在上面已经 break 了，不需要额外处理尾部
        }
        _spanPool.Delete(span);
        SystemFree(ptr, alloc_pages);
        return nullptr;
    }

    // 映射成功，正常返回或入库
    if (k >= NPAGES) {
        span->_isUse = true;
        return span;
    } else {
        _spanLists[span->_n].PushFront(span);
        _totalFreePages += span->_n;
        // 递归重试，这次必然命中 Phase 1
    }
    }
}

void PageCacheShard::ReleaseSpan(Span* span) noexcept {
    std::lock_guard<KzSTL::SpinMutex> lock(_mutex);

    // 合并逻辑
    // 注意：我们需要处理 Hot 和 Cold 的混合合并
    // 由于 Release 流程保证了 Span 回到原 Shard，
    // 如果 leftSpan 也是空闲的，且 SystemAlloc 具有物理连续性，
    // 那么 leftSpan 必然也属于当前 Shard 管理。
    // 向左合并
    while (true) {
        PAGE_ID leftId = span->_pageId - 1;
        Span* leftSpan = PageMap::GetInstance()->get(leftId);
        
        // 禁止跨分片合并
        // 必须检查 leftSpan->_shardId == _shardId
        if (leftSpan == nullptr || leftSpan->_isUse || leftSpan->_shardId != _shardId) break;

        // 摘除邻居 (无论它在 Hot 还是 Cold 容器中)
        leftSpan->Remove();
        
        // 只有 Hot 的邻居才占用 _totalFreePages
        // 如果邻居是 Cold 的，它没贡献物理内存计数，所以不能减
        if (!leftSpan->_isCold) {
            _totalFreePages -= leftSpan->_n;
        }
        
        span->_pageId = leftSpan->_pageId;
        span->_n += leftSpan->_n;
        _spanPool.Delete(leftSpan);
    }

    // 向右合并
    while (true) {
        PAGE_ID rightId = span->_pageId + span->_n;
        Span* rightSpan = PageMap::GetInstance()->get(rightId);

        // 禁止跨分片合并
        if (rightSpan == nullptr || rightSpan->_isUse || rightSpan->_shardId != _shardId) break;

        rightSpan->Remove();
        
        if (!rightSpan->_isCold) {
            _totalFreePages -= rightSpan->_n;
        }
        
        span->_n += rightSpan->_n;
        _spanPool.Delete(rightSpan);
    }

    // ============================================================
    // 归还逻辑
    // ============================================================
    
    span->_isUse = false;
    
    // 合并后的 Span 算 Hot 还是 Cold？
    // 策略：只要发生合并，或者归还，我们暂时都视为 "Hot"。
    // 理由：虽然它可能包含 Cold 的部分，但我们把它拉回了活动链表。
    // 如果它很大且长时间不用，触发 ReleaseSomeSpansToSystem 时会再次将其变 Cold。
    span->_isCold = false; 
    
    // 闲置 Span 只映射首尾，节省 Radix Tree 压力
    // 架构保证：这里的 set 绝对不会失败，因为 Radix 节点在 NewSpan 时已分配
    PageMap::GetInstance()->set(span->_pageId, span);
    PageMap::GetInstance()->set(span->_pageId + span->_n - 1, span);

    if (span->_n < NPAGES) {
        _spanLists[span->_n].PushFront(span);
    } else {
        _largeSpanLists[span->_n].PushFront(span);
    }
    
    _totalFreePages += span->_n; // 视为 Hot，增加计数

    // ============================================================
    // 触发回收
    // ============================================================
    if (_totalFreePages > _releaseThreshold) {
        ReleaseSomeSpansToSystem();
    }
}

void PageCacheShard::ReleaseSomeSpansToSystem() noexcept {
    // 1. 优先回收大对象 (Hot Map -> Cold Map)
    while (_totalFreePages > _releaseThreshold && !_largeSpanLists.empty()) {
        // 取出最大的 SpanList
        auto it = _largeSpanLists.rbegin();
        
        // 注意：反向迭代器删除需要技巧，或者转为正向迭代器
        // _largeSpanLists.erase(std::next(it).base()); // C++11 way
        // 或者更简单：保存 key，退出迭代器再删除
        // 检查并清理空的 Map 节点
        if (it->second.Empty()) {
            _largeSpanLists.erase(std::next(it).base()); // 删除key
            continue;
        }

        Span* span = it->second.PopFront();
        // 如果取出来是空的（僵尸节点），直接跳过，继续循环
        // 此时上面的 erase 已经把它清理掉了，下次循环不会再遇到它
        if (span == nullptr) {
            continue; 
        }

        ReleaseSpanToCold(span);
    }

    // 2. 其次回收小对象 (Hot Array -> Cold Array)
    if (_totalFreePages > _releaseThreshold) {
        for (size_t i = NPAGES - 1; i > 0; --i) {
            SpanList& list = _spanLists[i];
            
            while (_totalFreePages > _releaseThreshold && !list.Empty()) {
                Span* span = list.PopFront();
                ReleaseSpanToCold(span);
            }
            
            // 如果水位已经降下来了，提前退出循环，不再清理更小的桶
            // 保护像 1页、2页 这种极度热点的数据不被轻易回收
            if (_totalFreePages <= _releaseThreshold) break;
        }
    }
}

void PageCacheShard::ReleaseSpanToCold(Span* span) noexcept {
    // 1. 状态变更
    _totalFreePages -= span->_n; // 从 Hot 计数扣除
    span->_isCold = true;        // 标记为冷

    // 2. Madvise 释放物理内存
    void* ptr = (void*)(span->_pageId << PAGE_SHIFT);
#ifdef _WIN32
    VirtualFree(ptr, span->_n << PAGE_SHIFT, MEM_DECOMMIT);
#else
    madvise(ptr, span->_n << PAGE_SHIFT, MADV_DONTNEED);
    // printf("[DEBUG] Releasing Cold Span: ptr=%p, pages=%zu. Using madvise.\n", ptr, span->_n);
#endif

    // 3. 挂入 Cold 容器
    if (span->_n < NPAGES) {
        _releasedSpanLists[span->_n].PushFront(span);
    } else {
        _releasedLargeSpanLists[span->_n].PushFront(span);
    }
    
    // 注意：Span 在 PageMap 中的映射保持不变！
    // 这样邻居在合并时，依然可以通过 PageMap 找到这个 Cold Span
}

Span* PageCacheShard::AllocFromHotList(SpanList& list, size_t k) noexcept {
    Span* span = list.PopFront();
    
    // 出库
    _totalFreePages -= span->_n;

    // 切分逻辑
    if (span->_n > k) {
        Span* split = _spanPool.New();
        if (!split) [[unlikely]] { 
            list.PushFront(span); 
            _totalFreePages += span->_n;
            return nullptr; 
        }
        split->_pageId = span->_pageId + k;
        split->_n = span->_n - k;
        split->_isCold = false; // 剩下的也是热的
        split->_shardId = _shardId;

        span->_n = k;

        // 剩下的挂回 Hot List
        _spanLists[split->_n].PushFront(split);
        _totalFreePages += split->_n; // 回库

        // 架构保证：切分操作的 set 绝对不会失败
        PageMap::GetInstance()->set(split->_pageId, split);
        PageMap::GetInstance()->set(split->_pageId + split->_n - 1, split);
    }

    // 建立映射
    for (size_t i = 0; i < k; ++i) PageMap::GetInstance()->set(span->_pageId + i, span);
    
    span->_isUse = true;
    span->_isCold = false;
    return span;
}

Span* PageCacheShard::AllocFromColdList(SpanList& list, size_t k) noexcept {
    Span* span = list.PopFront();
    
    // Cold Span 不占用 _totalFreePages，所以不需要减计数
    // 但当它被分配出去后，用户写入数据，它会变热。
    // 这里我们不需要加 _totalFreePages，因为它直接变成了 _isUse=true

    // 切分逻辑
    if (span->_n > k) {
        Span* split = _spanPool.New();
        if (!split) [[unlikely]] { 
            list.PushFront(span); 
            return nullptr; 
        }
        split->_pageId = span->_pageId + k;
        split->_n = span->_n - k;
        split->_isCold = true; // 剩下的依然是冷的
        split->_shardId = _shardId;

        span->_n = k;

        // 剩下的挂回 Cold List
        _releasedSpanLists[split->_n].PushFront(split);
        
        // Cold 的 Split 也需要维护首尾映射，方便合并
        // 架构保证：切分操作的 set 绝对不会失败
        PageMap::GetInstance()->set(split->_pageId, split);
        PageMap::GetInstance()->set(split->_pageId + split->_n - 1, split);
    }

    // 建立映射
    for (size_t i = 0; i < k; ++i) PageMap::GetInstance()->set(span->_pageId + i, span);
    
#ifdef _WIN32
    // 重新提交物理内存
    void* ptr = reinterpret_cast<void*>(span->_pageId << PAGE_SHIFT);
    VirtualAlloc(ptr, span->_n << PAGE_SHIFT, MEM_COMMIT, PAGE_READWRITE);
#endif
    span->_isUse = true;
    span->_isCold = false; // 激活：变热
    return span;
}

template<typename MapType>
Span* PageCacheShard::AllocFromMap(MapType& map, typename MapType::iterator it, size_t k, bool isCold) noexcept {
    Span* span = it->second.PopFront();

    assert(span != nullptr); // 理论上不会发生, 仅防御一下

    // 如果是从 Hot Map 拿的，需要减计数
    if (!isCold) {
        _totalFreePages -= span->_n;
    }

    if (span->_n > k) {
        Span* split = _spanPool.New();
        if (!split) [[unlikely]] { 
            it->second.PushFront(span);
            if (!isCold) _totalFreePages += span->_n;
            return nullptr; 
        }
        split->_pageId = span->_pageId + k;
        split->_n = span->_n - k;
        split->_isCold = isCold; // 继承来源的冷热属性
        split->_shardId = _shardId;

        span->_n = k;

        // 剩下的挂回对应的 Map (Hot->Hot, Cold->Cold)
        // 这里为了简单，我们根据 isCold 标志决定挂哪里
        // 注意：AllocFromMap 是模板，但挂回逻辑需要具体对象
        if (isCold) {
            if (split->_n < NPAGES) {
                _releasedSpanLists[split->_n].PushFront(split);
            } else {
                _releasedLargeSpanLists[split->_n].PushFront(split);
            }
        } else {
            if (split->_n < NPAGES) {
                _spanLists[split->_n].PushFront(split);
            } else {
                _largeSpanLists[split->_n].PushFront(split);
            }
            _totalFreePages += split->_n;
        }

        PageMap::GetInstance()->set(split->_pageId, split);
        PageMap::GetInstance()->set(split->_pageId + split->_n - 1, split);
    }

    for (size_t i = 0; i < k; ++i) PageMap::GetInstance()->set(span->_pageId + i, span);
    
#ifdef _WIN32
    // 重新提交物理内存
    void* ptr = reinterpret_cast<void*>(span->_pageId << PAGE_SHIFT);
    VirtualAlloc(ptr, span->_n << PAGE_SHIFT, MEM_COMMIT, PAGE_READWRITE);
#endif
    span->_isUse = true;
    span->_isCold = false;

    // 延迟到最后清理空节点，防止 split 失败时迭代器失效导致 UAF
    if (it->second.Empty()) {
        map.erase(it);
    }
    return span;
}

} // namespace KzAlloc