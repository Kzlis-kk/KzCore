#pragma once
#include "Common.h"
#include "ObjectPool.h"

namespace KzAlloc {
// 定义链表节点基类 (只包含指针)
struct SpanLink {
    SpanLink* _next = nullptr; // 双向链表结构
    SpanLink* _prev = nullptr;
};

// 管理 Span 的核心结构体 (也是双向链表节点)
struct Span : public SpanLink {

    PAGE_ID _pageId = 0;   // 页号
    size_t  _n = 0;        // 页数
    
    size_t  _objSize = 0;      // 切分的小对象大小 (CentralCache 使用)
    size_t  _useCount = 0;     // 分配出去的小对象数量
    void* _freeList = nullptr; // 切好小对象的空闲链表
    
    bool    _isUse = false;    // true: 在 CentralCache/用户手中; false: 在 PageCache 中
    bool   _isCold = false;    // 标记是否为冷数据 (物理内存已释放，但虚拟地址保留)
    
    // 记录该 Span 属于哪个 PageCacheShard，防止跨分片死锁
    uint8_t _shardId = 0;

    void Remove() noexcept {
        _prev->_next = _next;
        _next->_prev = _prev;
        _prev = nullptr;
        _next = nullptr;
    }
};

// 双向链表容器 (带哨兵位)
struct SpanList {
public:

    SpanList() noexcept {
        _head._next = &_head;
        _head._prev = &_head;
    } 

    ~SpanList() = default;
    
    // 禁用拷贝
    SpanList(const SpanList&) = delete;
    SpanList& operator=(const SpanList&) = delete;

    // 允许移动构造
    // std::map 在旋转平衡树时需要移动节点
    SpanList(SpanList&& other) noexcept {
        if (other.Empty()) [[unlikely]] {
            _head._next = &_head;
            _head._prev = &_head;
        } 
        else {
            _head._next = other._head._next;
            _head._prev = other._head._prev;
            _head._next->_prev = &_head;
            _head._prev->_next = &_head;
        
            // 将 other 彻底置空
            other._head._next = &other._head;
            other._head._prev = &other._head;
        }
    }

    // 允许移动赋值
    SpanList& operator=(SpanList&& other) noexcept {
        if (this != &other) {
            if (other.Empty()) [[unlikely]] {
                _head._next = &_head;
                _head._prev = &_head;
            } 
            else {
                _head._next = other._head._next;
                _head._prev = other._head._prev;
                _head._next->_prev = &_head;
                _head._prev->_next = &_head;
        
                // 将 other 彻底置空
                other._head._next = &other._head;
                other._head._prev = &other._head;
            }
        }
        return *this;
    }

    Span* Begin() noexcept { 
        return static_cast<Span*>(_head._next); 
    }
    Span* End() noexcept { 
        return static_cast<Span*>(&_head); 
    }
    bool Empty() const noexcept { 
        return _head._next == &_head; 
    }

    void PushFront(Span* span) noexcept {
        assert(&_head);
        Insert(Begin(), span);
    }
    
    // 弹出并返回首个节点 (如果空则返回 nullptr)
    [[nodiscard]] Span* PopFront() noexcept {
        Span* front = Begin();
        if (front == End()) {
            return nullptr;
        }
        Erase(front);
        return front;
    }
    
    // 在 pos 之前插入 span
    void Insert(Span* pos, Span* span) noexcept {
        assert(pos);
        assert(span);
        
        SpanLink* prev = pos->_prev;
        
        prev->_next = span;
        span->_prev = prev;
        
        span->_next = pos;
        pos->_prev = span;
    }
    
    // 移除指定节点 (并不释放内存，只是从链表解绑)
    void Erase(Span* span) noexcept {
        assert(span);
        assert(span != End());
        
        SpanLink* prev = span->_prev;
        SpanLink* next = span->_next;
        
        prev->_next = next;
        next->_prev = prev;
        
        // 断开连接，防野指针
        span->_prev = nullptr;
        span->_next = nullptr;
    }

private:
    SpanLink _head; // 哨兵节点
};

} // namespace KzAlloc