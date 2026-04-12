#pragma once

#include <mutex>
#include <new>
#include "Common.h"
#include "PageCache.h"
#include "KzSTL/SpinMutex.h"

namespace KzAlloc {

// 将锁的类型泛型化，这里为了测试就这样设计了，实际也确实是自旋锁更好
// 默认使用 SpinMutex，如果想测试 std::mutex 也可以直接换
// 综合来说自旋锁性能会好些，因为大部分都是高速链表操作。
// 但是在申请内存的时候可能会导致CPU大量空转，这时互斥锁会好些，不过，我们会在申请内存前手动释放自旋锁，所以也差不了
// 并且，由于我们尽力降低了并发冲突，所以自旋锁就是最优解
template <class LockType>
struct alignas(kCacheLineSize) SpanListBucket : public SpanList {
    LockType _mutex;
};

// 单例模式：中心缓存
class CentralCache {
public:
    static CentralCache* GetInstance() {
    alignas (CentralCache) static char _buffer[sizeof(CentralCache)];
    static CentralCache* _instance = nullptr;
    
    static const bool _inited = [&]() {
        _instance = new (_buffer) CentralCache();
        return true;
    }();
    
    static_cast<void>(_inited);
    return _instance;
    }

    // 从中心缓存获取一定数量的对象给 ThreadCache
    size_t FetchRangeObj(void*& start, void*& end, size_t n, size_t size) noexcept;

    // 将 ThreadCache 归还的一串对象释放回对应的 Span
    void ReleaseListToSpans(void* start, size_t size) noexcept;

private:
    CentralCache() = default;
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    // 获取一个非空的 Span
    // 为了解耦，这里传入具体的 Bucket 类型
    Span* GetOneSpan(SpanListBucket<KzSTL::SpinMutex>& bucket, size_t size) noexcept;

private:
    // 这里显式指定使用 SpinMutex
    std::array<SpanListBucket<KzSTL::SpinMutex>, MAX_NFREELISTS> _spanLists;
};

namespace detail {
// 慢启动算法 (根据对齐后的大小计算应该一次性申请多少个 aligned_size 大小内存)
inline size_t CalculateFetchBatchSize(size_t aligned_size) noexcept {
    size_t num = MAX_BYTES / aligned_size; 
    if (num == 0) num = 1;
    if (num > 512) num = 512;
    return num;
}

// 计算申请页数
// 此函数现在假设传入的已经是 aligned_size
inline size_t CalculatePageNeed(size_t aligned_size) noexcept {
    size_t num = CalculateFetchBatchSize(aligned_size);
    size_t npage = (num * aligned_size + PAGE_ROUND_UP_NUM) >> PAGE_SHIFT;
    if (npage == 0) npage = 1;
    return npage;
}
} // namespace detail

} // namespace KzAlloc