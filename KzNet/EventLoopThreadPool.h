#pragma once

#include <vector>
#include <memory>
#include <string>

#include "KzAlloc/KzAllocator.h"
#include "KzSTL/Function.h"
#include "KzSTL/KzString.h"
#include "EventLoop.h"
#include "EventLoopThread.h"

using namespace std::string_view_literals;

namespace KzNet {

/**
 * @brief IO 线程池 (Load Balancer)
 * 
 * * 核心职责：
 *   1. 管理所有 Sub-Reactor 线程 (EventLoopThread)。
 *   2. 提供 getNextLoop() 接口，将新连接分发给 Sub-Reactor。
 * 
 * * 性能优化：
 *   1. 连续内存存储 EventLoopThread，减少 Cache Miss。
 *   2. 使用位运算/乘法优化 Round-Robin 取模操作。
 */
class EventLoopThreadPool {
public:
    using ThreadInitCallback = KzSTL::Function<void(EventLoop*)>;

    EventLoopThreadPool(EventLoop* baseLoop, size_t threadNum = 8) noexcept
        : _baseLoop(baseLoop),
          _threadNum(threadNum)
    {}

    ~EventLoopThreadPool() {
        if (_threads) {
            // 手动调用每个对象的析构函数
            for (size_t i = 0; i < _threadNum; ++i) {
                _threads[i].~EventLoopThread();
            }
            // 释放整块内存
            KzAlloc::free_aligned(_threads, sizeof(EventLoopThread) * _threadNum, alignof(EventLoopThread));
        }
    }

    void setThreadNum(size_t threadNum) noexcept { _threadNum = threadNum; }

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;
    
    void start(int first_cpu = 0, const ThreadInitCallback& cb = ThreadInitCallback(), std::string_view name = "EventLoopThreadPool"sv) noexcept {
        assert(!_started);
        assert(_baseLoop->isInLoopThread());

        _started = true;

        // 申请连续大内存
        size_t totalSize = sizeof(EventLoopThread) * _threadNum;
        void* rawMem = KzAlloc::malloc_aligned(totalSize, alignof(EventLoopThread));
        if (!rawMem) [[unlikely]] KzAlloc::handleOOM();

        _threads = static_cast<EventLoopThread*>(rawMem);

        // 构造并启动线程
        for (size_t i = 0; i < _threadNum; ++i) {
            char idBuf[64];
            // 这里的 memcpy 需要注意边界
            size_t nameLen = std::min(name.size(), sizeof(idBuf) - 20);
            ::memcpy(idBuf, name.data(), nameLen);
            char* end = KzAlgorithm::Jeaiii::to_chars(idBuf + nameLen, i);
            *end = '\0';
                
            // Placement New
            new (&_threads[i]) EventLoopThread();
                
            // 启动线程 (同步等待初始化完成)
            _threads[i].startLoop(i + first_cpu, cb, std::string_view(idBuf, end - idBuf));
        }
    }


    // === 核心负载均衡接口 ===
    
    /**
     * @brief 获取下一个 Loop (Round-Robin)
     * * 热路径：每个新连接都会调用。
     * * 优化：避免取模运算。
     */
    EventLoop* getNextLoop() noexcept {
        assert(_baseLoop->isInLoopThread());
        assert(_started);

        EventLoop* loop = _baseLoop;

        // 如果 _threadNum 为 0，永远返回 baseLoop
        if (_threadNum == 0) [[unlikely]] {
            return loop;
        }

        assert(_threads);

        size_t currentIdx = _next;
        
        if ((_threadNum & (_threadNum - 1)) == 0) {
            loop = _threads[currentIdx & (_threadNum - 1)].getLoop();
            _next++; 
        } else {
            loop = _threads[currentIdx].getLoop();
            _next++;
            if (_next >= _threadNum) {
                _next = 0;
            }
        }

        return loop;
    }

    /**
     * @brief 获取特定 Hash 的 Loop (Consistent Hash)
     * * 场景：源 IP 哈希调度，保证同一 IP 总是落到同一个 Loop。
     */
    EventLoop* getLoopForHash(size_t hashCode) noexcept {
        assert(_baseLoop->isInLoopThread());
        
        if (_threadNum == 0) [[unlikely]] return _baseLoop;

        assert(_threads);

        if ((_threadNum & (_threadNum - 1)) == 0) {
            return _threads[hashCode & (_threadNum - 1)].getLoop();
        }
        else {
            return _threads[hashCode % _threadNum].getLoop();
        }
    }

    // 获取所有 Loop (用于广播)
    std::vector<EventLoop*> getAllLoops() noexcept {
        assert(_baseLoop->isInLoopThread());
        std::vector<EventLoop*> loops;
        
        if  (_threadNum == 0) [[unlikely]] {
            loops.push_back(_baseLoop);
        } else {
            assert(_threads != nullptr);
            loops.reserve(_threadNum);
            for (size_t i = 0; i < _threadNum; ++i) {
                loops.push_back(_threads[i].getLoop());    
            }
        }
        return loops;
    }


    bool started() const noexcept { return _started; }

private:
    EventLoop* _baseLoop; // 主 Reactor (Acceptor 所属)
    bool _started = false;
    size_t _next = 0; // 轮询索引
    size_t _threadNum;
    
    EventLoopThread* _threads = nullptr;
};

} // namespace KzNet