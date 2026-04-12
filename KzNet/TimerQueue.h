#pragma once

#include "KzTimer/Timer.h"
#include "KzTimer/TimerId.h"
#include "KzTimer/TimerList.h"
#include "KzTimer/TimeStamp.h"
#include "KzTimer/TimerRequest.h"
#include "KzAlloc/KzAllocator.h"
#include "KzSTL/Job.h"
#include "KzSTL/Task.h"
#include "KzThread/ThreadPool.h"
#include <array>
#include <vector>
#include <cstring>
#include <unistd.h>

using namespace KzTimer;

namespace KzNet {

class EventLoop;

class TimerQueue {
public:
    using Job = KzSTL::Job<void(), 48>;
    using TimerIdVector = std::vector<TimerId, KzAlloc::KzAllocator<TimerId>>;
    using TimerReqVector = std::vector<TimerRequest, KzAlloc::KzAllocator<TimerRequest>>;

    TimerQueue(EventLoop* loop) noexcept
    : _loop(loop),
      _startTimeNs(TimeStamp::now().nanoseconds()),
      _currentTicks(0),
      _timerCount(0),
      _freeHead(-1)
{
    _handles.reserve(1024);
}

    ~TimerQueue() noexcept {
        // 遍历时间轮，释放所有 Timer
        for (auto& list: _wheels) {
            while (Timer* timer = list.pop_front()) {
                timer->~Timer();
                KzAlloc::free(timer, sizeof(Timer));
            }
        }
    }

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    // 供 EventLoop 在 epoll_wait 唤醒后调用，驱动时间轮运转
    void advanceTo(TimeStamp now) noexcept;

    // 供 EventLoop 判断 epoll_wait 的 timeout 参数
    bool hasTimers() const noexcept { return _timerCount > 0; }

    // ========================================================================
    // 单个定时器接口
    // ========================================================================

    // [IO线程专用] 立即返回 ID
    TimerId addTimerInLoop(Job&& cb, TimeStamp when, int64_t interval, bool strict = false) noexcept;

    // [跨线程] 极速版：射后不理 (Fire-and-Forget)，不返回 ID
    void addTimerDetached(Job&& cb, TimeStamp when, int64_t interval, bool strict = false) noexcept;
    // [跨线程] 协程版：挂起当前协程，直到 IO 线程分配好 ID 后恢复
    KzSTL::Task<TimerId> addTimerAsync(KzThread::ThreadPool* currentPool,
        Job&& cb, TimeStamp when, int64_t interval, bool strict = false) noexcept;

    // [通用] 取消定时器 (线程安全)
    void cancel(TimerId timerId) noexcept;
    void cancel(const TimerIdVector& timerIds) noexcept;

    // ========================================================================
    // 批量定时器接口
    // ========================================================================

    // [跨线程] 协程版批量
    KzSTL::Task<TimerIdVector> addTimersAsync(KzThread::ThreadPool* currentPool,
        TimerReqVector&& reqs) noexcept;
    
    // [跨线程] 极速版批量
    void addTimersDetached(TimerReqVector&& reqs) noexcept;

    // IO 线程专用 插入批量 Timer (返回 ID 列表)
    TimerIdVector addTimersInLoop(TimerReqVector&& reqs) noexcept;

private:
    // === 内部核心逻辑 (必须在 IO 线程运行) ===
    void processExpiredTimers() noexcept;

    // 插入单个 Timer (返回 ID)
    TimerId addTimerInLoop(Timer* timer) noexcept;

    // 插入单个 Timer (不返回 ID)
    void addTimerDetachedInLoop(Timer* timer) noexcept;

    // 插入批量 Timer (不返回 ID 列表)
    void addTimersDetachedInLoop(TimerReqVector&& reqs) noexcept;

    // 资源回收
    void freeTimer(Timer* timer) noexcept;

    // === 句柄表与空闲链表 ===
    struct HandleEntry {
        union {
            Timer* _timer;
            int32_t _nextFreeIdx;
        };
        int64_t _sequence;
    };
    int32_t allocSlot(Timer* timer) noexcept;
    void freeSlot(int32_t index) noexcept;

    // === 协程 Awaiter 定义 (嵌套私有类) ===
    struct AddTimerAwaiter;
    struct AddTimersAwaiter;

private:
    // 时间轮配置
    static constexpr int kWheelSize = 4096;
    static constexpr int kWheelMask = kWheelSize - 1;
    static constexpr int64_t kTickNs = 10 * 1000 * 1000; // 10ms 1次

    KzNet::EventLoop* _loop;

    int64_t _startTimeNs;  // 绝对时间，防止时间轮漂移
    int64_t _currentTicks;
    size_t _timerCount;
    int32_t _freeHead;
    // 静态数组时间轮 (内嵌)
    std::array<TimerList, kWheelSize> _wheels;
    // 句柄表 (KzAlloc)
    std::vector<HandleEntry, KzAlloc::KzAllocator<HandleEntry>> _handles;
};

// === 句柄表管理 (O(1)) ===
inline int32_t TimerQueue::allocSlot(Timer* timer) noexcept {
    int32_t index;
    if (_freeHead != -1) {
        index = _freeHead;
        _freeHead = _handles[index]._nextFreeIdx;
    }
    else {
        index = static_cast<int32_t>(_handles.size());
        _handles.push_back({}); // 直接 push_back 把扩容逻辑交给 vector
    }

    _handles[index]._timer = timer;
    _handles[index]._sequence = timer->_sequence;

    return index;
}

inline void TimerQueue::freeSlot(int32_t index) noexcept {
    _handles[index]._sequence = -1; // 废弃版本号
    _handles[index]._nextFreeIdx = _freeHead;
    _freeHead = index;
}

inline void TimerQueue::freeTimer(Timer* timer) noexcept {
    if (timer->_handleIndex >= 0) {
        freeSlot(timer->_handleIndex);
    }

    timer->~Timer();
    KzAlloc::free(timer, sizeof(Timer));
}

} // namespace KzNet