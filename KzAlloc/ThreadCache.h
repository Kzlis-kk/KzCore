#pragma once

#include "Common.h"
#include "CentralCache.h"

namespace KzAlloc {

// 专门为 ThreadCache 设计的轻量级单向自由链表
// 记录了 tail 和 size，支持 O(1) 的区间插入和删除
class FreeList {
public:
    void Push(void* obj) noexcept {
        assert(obj);
        // 头插法
        SetNextObj(obj, _head);
        _head = obj;
        
        // 如果是第一个节点，tail 也是它
        if (_tail == nullptr) {
            _tail = obj;
        }
        _size++;
    }

    void* Pop() noexcept {
        assert(_head);
        void* obj = _head;
        _head = GetNextObj(_head);
        
        if (_head == nullptr) {
            _tail = nullptr;
        }
        _size--;
        
        return obj;
    }

    // O(1) 插入一段链表
    // start: 链表头, end: 链表尾, n: 数量
    void PushRange(void* start, void* end, size_t n) noexcept {
        assert(start && end);
        
        // 把新链表的尾巴接到旧链表的头
        SetNextObj(end, _head);
        _head = start;

        // 如果旧链表为空，更新 tail
        if (_tail == nullptr) {
            _tail = end;
        }
        
        _size += n;
    }

    // O(1) 弹出一批对象
    // n: 期望弹出的数量
    // start, end: 输出参数
    void PopRange(void*& start, void*& end, size_t n) noexcept {
        assert(n <= _size);

        // 在这里循环 (n 通常不大，且是在回收路径，可接受)
        start = _head;
        // 走 n-1 步找到 end
        end = _head;
        for (size_t i = 0; i < n - 1; ++i) {
            end = GetNextObj(end);
        }
        
        // 记录新的 head
       _head = GetNextObj(end);
        
        // 断开 end 的连接
        SetNextObj(end, nullptr);
        
        // 如果取空了，更新 tail
        if (_head == nullptr) {
            _tail = nullptr;
        }
        
        _size -= n;
    }

    bool Empty() const noexcept { return _head == nullptr; }
    size_t Size() const noexcept { return _size; }
    size_t MaxSize() const noexcept { return _maxSize; }
    size_t MaxNum() const noexcept { return _maxNum; }
    void SetMaxSize(size_t maxSize) noexcept { _maxSize = maxSize; }
    void SetMaxNum(size_t maxNum) noexcept { _maxNum = maxNum; }


private:
    void* _head = nullptr;
    void* _tail = nullptr; // 记录尾指针，方便 PushRange O(1)
    size_t _size = 0;      // 当前链表长度
    size_t _maxSize = 1;   // 慢启动阈值 (限制该链表最大能挂多少个)
    size_t _maxNum;        // _maxSize上限
};

class ThreadCache {
public:

    explicit ThreadCache() noexcept {
       for (int i = 0; i < MAX_NFREELISTS; ++i) {
        _freeLists[i].SetMaxNum(SizeUtils::NumMoveSize(i));
       }
    }
    ~ThreadCache() {
    for (int i = 0; i < MAX_NFREELISTS; ++i) {
        FreeList& list = _freeLists[i];
        if (!list.Empty()) {
            void* start = nullptr;
            void* end = nullptr;
            size_t size = SizeUtils::Size(i);
            
            // 把整个链表全部弹出来
            list.PopRange(start, end, list.Size());
            
            // 归还给 CentralCache
            CentralCache::GetInstance()->ReleaseListToSpans(start, size);
        }
    }
}

    // 申请内存
    void* Allocate(size_t size) noexcept;

    // 释放内存
    void Deallocate(void* ptr, size_t size) noexcept;

    // 从 CentralCache 获取对象
    void* FetchFromCentralCache(size_t index, size_t size) noexcept;

    // 释放过多内存给 CentralCache
    void ListTooLong(FreeList& list, size_t size) noexcept;

private:
    // 哈希桶，对应 SizeUtils 的映射规则
    std::array<FreeList, MAX_NFREELISTS> _freeLists;
};

inline void* ThreadCache::Allocate(size_t size) noexcept {
    // 计算桶索引
    int index = SizeUtils::Index(size);
    FreeList& list = _freeLists[index];

    // 优先从 FreeList 拿
    if (!list.Empty()) [[likely]] {
        return list.Pop();
    }

    // 没货了，找 CentralCache 进货 (Cold Path)
    return FetchFromCentralCache(index, size);
}

inline void ThreadCache::Deallocate(void* ptr, size_t size) noexcept {
    assert(ptr);

    // 计算桶索引
    int index = SizeUtils::Index(size);

    FreeList& list = _freeLists[index];

    // 归还给 FreeList
    list.Push(ptr);

    // 检测是否囤积了太多内存
    // 如果当前链表长度 > 慢启动阈值，说明该线程可能只是短时间突发分配，
    // 现在不需要这么多了，归还一部分给 CentralCache，避免内存泄露式占用。
    if (list.Size() >= list.MaxSize() + list.MaxNum()) {
        ListTooLong(list, size);
    }
}

} // namespace KzAlloc