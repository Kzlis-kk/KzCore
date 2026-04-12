#include "CentralCache.h"

namespace KzAlloc {

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

size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t n, size_t size) noexcept {
    // Hot Path：直接使用 raw_size 查表
    // SizeUtils 保证了 Index(raw_size) == Index(aligned_size)
    // 避免了 RoundUp 的开销，因为如果桶里有货，我们根本不需要知道 aligned_size
    int index = SizeUtils::Index(size);
    auto& bucket = _spanLists[index];

    // 加自旋锁
    bucket._mutex.lock();

    // 尝试获取 Span
    // 注意：这里传入的是 raw_size，因为 GetOneSpan 只有在真要申请内存时才需要对齐
    Span* span = GetOneSpan(bucket, size);
    if (!span) [[unlikely]] return 0;
    assert(span->_freeList);

    // 从链表提取对象
    // 这里的遍历不可避免，因为 span->_freeList 可能是乱序归还的，物理上不连续
    start = span->_freeList;
    end = start;
    size_t actualNum = 1;
    
    while (actualNum < n && GetNextObj(end) != nullptr) {
        void* next = GetNextObj(end);
        __builtin_prefetch(GetNextObj(next), 0, 3);
        end = next;
        actualNum++;
    }

    span->_freeList = GetNextObj(end);
    SetNextObj(end, nullptr);
    span->_useCount += actualNum;

    bucket._mutex.unlock();

    return actualNum; 
}

// 入参是 raw_size
Span* CentralCache::GetOneSpan(SpanListBucket<KzSTL::SpinMutex>& bucket, size_t size) noexcept {
    // 尝试从桶中查找现成的 Span
    Span* it = bucket.Begin();
    while (it != bucket.End()) {
        if (it->_freeList != nullptr) {
            return it;
        }
        it = static_cast<Span*>(it->_next);
    }

    // 桶空了，进入Cold Path -> 也就是这里才需要对齐
    // 先解锁
    bucket._mutex.unlock();

    // 在此处进行对齐
    // 这是整个分配路径中唯一一次调用 RoundUp
    size_t aligned_size = SizeUtils::RoundUp(size); 
    size_t kPages = detail::CalculatePageNeed(aligned_size);
    
    Span* span = PageHeap::GetInstance()->NewSpan(kPages);
    if (!span) [[unlikely]] return nullptr;
    span->_isUse = true;
    span->_objSize = aligned_size; // 记录对齐后的大小

    // 切分内存
    // 使用 aligned_size 进行切分，保证无碎片
    char* start = reinterpret_cast<char*>(span->_pageId << PAGE_SHIFT);
    size_t bytes = span->_n << PAGE_SHIFT; 
    char* end = start + bytes - aligned_size; // 减一个size避免剩余10字节(内存碎片)但却要分配16字节这种情况

    // 初始化链表结构
    // 虽然物理连续，但必须写入 Next 指针，ThreadCache 才能识别
    span->_freeList = start;
    void* tail = start;
    char* cur = start + aligned_size;
    
    // 这里依然需要循环写入指针
    // 虽然我们知道总量是 bytes / aligned_size (乘除法关系)
    // 但我们需要把每一个节点的头部写上 data，这个 O(N) 的内存写入无法省略
    while (cur <= end) {
        SetNextObj(tail, cur); 
        tail = cur;          
        cur += aligned_size;
    }
    SetNextObj(tail, nullptr);

    // 重新加锁
    bucket._mutex.lock();
    bucket.PushFront(span);

    return span;
}

void CentralCache::ReleaseListToSpans(void* start, size_t size) noexcept {
    // 这里使用 raw_size 查表也是安全的
    int index = SizeUtils::Index(size);
    auto& bucket = _spanLists[index];

    // 栈上微型哈希表/批处理缓存 (绝对不分配堆内存)
    constexpr int MAX_BATCH = 16;
    struct Batch {
        Span* span;
        void* head;
        void* tail;
        size_t count;
    };
    std::array<Batch, MAX_BATCH> batches;
    int batch_cnt = 0;

    auto flush_batches = [&]() {
        if (batch_cnt == 0) return;
        
        // 用于收集需要归还给 PageHeap 的 Span
        Span* spans_to_release[MAX_BATCH];
        int release_cnt = 0;
        // 锁内操作，绝对不中途解锁
        bucket._mutex.lock();
        for (int i = 0; i < batch_cnt; ++i) {
            Span* span = batches[i].span;
            
            // 将这一批链表挂回 Span
            SetNextObj(batches[i].tail, span->_freeList);
            span->_freeList = batches[i].head;
            span->_useCount -= batches[i].count;

            if (span->_useCount == 0) {
                bucket.Erase(span);
                span->_freeList = nullptr;
                span->_next = nullptr;
                span->_prev = nullptr;
                
                // 收集起来，稍后处理
                spans_to_release[release_cnt++] = span;
            }
        }
        bucket._mutex.unlock();

        for (int i = 0; i < release_cnt; ++i) {
            PageHeap::GetInstance()->ReleaseSpan(spans_to_release[i]);
        }
        batch_cnt = 0; // 重置
    };


    // 局部缓存 (类似 CPU 的 TLB)
    PAGE_ID last_id = 0;
    Span* last_span = nullptr;

    // 遍历链表，在锁外进行 Radix Tree 查询和分组
    while (start) {
        void* next = GetNextObj(start);
        // 立刻斩断当前节点与原链表的联系，防止脏指针残留
        SetNextObj(start, nullptr); 
        PAGE_ID id = reinterpret_cast<PAGE_ID>(start) >> PAGE_SHIFT;

        Span* span;
        if (id == last_id) [[likely]] {
            // 极速路径：命中局部缓存
            span = last_span;
        } else {
            // 慢速路径：查 Radix Tree(虽然是O(1)读但是内存访问次数过多，且一般不在CPU缓存中) 并更新缓存
            span = PageMap::GetInstance()->get(id);
            last_id = id;
            last_span = span;
        }

        // 查找是否已经在当前批次中
        bool found = false;
        for (int i = 0; i < batch_cnt; ++i) {
            if (batches[i].span == span) {
                // 头插法加入当前批次
                SetNextObj(start, batches[i].head);
                batches[i].head = start;
                batches[i].count++;
                found = true;
                break;
            }
        }

        // 如果没找到，开启新批次
        if (!found) {
            if (batch_cnt == MAX_BATCH) {
                flush_batches(); // 栈满了，刷入 CentralCache
            }
            batches[batch_cnt].span = span;
            batches[batch_cnt].head = start;
            batches[batch_cnt].tail = start;
            batches[batch_cnt].count = 1;
            batch_cnt++;
        }

        start = next;
    }

    // 处理剩余的批次
    flush_batches();
}

} // namespace KzAlloc