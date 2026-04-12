#pragma once

#include <vector>
#include <atomic>
#include <mutex>
#include <memory>
#include <array>
#include <iterator>
#include <span>
#include <functional>
#include <sys/eventfd.h>

#include "KzSTL/SpinMutex.h"
#include "KzThread/CurrentThread.h"
#include "KzThread/ThreadPool.h"
#include "KzSTL/Job.h"
#include "KzAlloc/KzAllocator.h"
#include "KzTimer/TimeStamp.h"
#include "KzTimer/TimerId.h"
#include "KzTimer/TimerRequest.h"
#include "KzSTL/ArrayMpscQueue.h"
#include "KzSTL/Task.h"
#include "Poller.h"
#include "Channel.h"
#include "Socket.h"
#include "TimerQueue.h"

namespace KzNet {

/**
 * @brief Reactor 核心事件循环 (High Performance & Robustness)
 * 
 * * 架构特点：
 *   1. One Loop Per Thread: 线程亲和性设计，核心操作无锁。
 *   2. Dual-Layer Task Queue: L1 无锁队列 + L2 兜底队列，兼顾吞吐与可靠性。
 *   3. Wakeup Coalescing: 聚合唤醒信号，减少 eventfd 系统调用。
 *   4. Stack Batching: 任务执行全程在栈上，零堆分配。
 */
class EventLoop {
public:
    using Job = KzSTL::Job<void(), 48>;

    explicit EventLoop() noexcept;
    ~EventLoop() noexcept;

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // === 核心控制 ===
    // 开始事件循环 (阻塞)
    void loop() noexcept;

    // 退出循环 (线程安全)
    void quit() noexcept;


    // === 任务调度 (核心优化点) ===
    /**
     * @brief 在 Loop 线程执行任务
     * * 如果当前是 Loop 线程，直接执行。
     * * 否则，加入队列并唤醒。
     */
    void runInLoop(Job&& cb) noexcept;

    /**
     * @brief 将任务放入队列
     * * 采用 L1(无锁) + L2(有锁) 双层架构。
     * * 采用唤醒聚合技术。
     */
    void queueInLoop(Job&& cb) noexcept;

    /**
     * @brief 批量提交任务
     * * 场景：从其他线程一次性迁移大量任务到 IO 线程。
     * * 优势：极大减少锁竞争 (L2) 和 原子操作次数 (L1)。
     * * 异常安全：如果 L1 队列满，自动降级到 L2 队列，保证不丢任务。
     */
    void queueInLoopBatch(std::span<Job> funcs) noexcept;


    // === 定时器接口 (转发给 TimerQueue) ===
    using TimerIdVector = std::vector<KzTimer::TimerId, KzAlloc::KzAllocator<KzTimer::TimerId>>;
    using TimerReqVector = std::vector<KzTimer::TimerRequest, KzAlloc::KzAllocator<KzTimer::TimerRequest>>;

    // 异步协程接口
    KzSTL::Task<KzTimer::TimerId> addTimerAsync(KzThread::ThreadPool* pool, Job&& cb,
         KzTimer::TimeStamp when, int64_t interval = 0, bool strict = false) noexcept {
        return _timerQueue.addTimerAsync(pool, std::move(cb), when, interval, strict);
    }

    // 同步接口
    KzTimer::TimerId addTimerInLoop(Job&& cb,
         KzTimer::TimeStamp when, int64_t interval = 0, bool strict = false) noexcept {
            return _timerQueue.addTimerInLoop(std::move(cb), when, interval, strict);
         }

    // 极速版 (Fire-and-Forget)
    void addTimerDetached(Job&& cb, KzTimer::TimeStamp when, int64_t interval = 0, bool strict = false) noexcept {
        _timerQueue.addTimerDetached(std::move(cb), when, interval, strict);
    }

    // [批量添加 - Detached]
    // 场景：批量设置超时，不需要 ID
    void addTimersDetached(TimerReqVector&& reqs) noexcept {
        _timerQueue.addTimersDetached(std::move(reqs));
    }

    TimerIdVector addTimersInLoop(TimerReqVector&& reqs) noexcept {
        _timerQueue.addTimersInLoop(std::move(reqs));
    }

    // [批量添加 - Async Coroutine]
    // 场景：在 Worker 协程中批量添加定时器并获取 ID
    KzSTL::Task<TimerIdVector> addTimersAsync(KzThread::ThreadPool* currentPool, TimerReqVector&& reqs) noexcept {
        return _timerQueue.addTimersAsync(currentPool, std::move(reqs));
    }

    void cancel(KzTimer::TimerId timerId) noexcept {
        _timerQueue.cancel(timerId);
    }
    void cancel(const TimerIdVector& timerIds) noexcept {
        _timerQueue.cancel(timerIds);
    }

    // === 内部接口 (由 Channel/Poller 调用) ===
    void updateChannel(Channel* channel) noexcept;
    void removeChannel(Channel* channel) noexcept;
    bool hasChannel(Channel* channel) noexcept;

    // 极速线程检查 (thread_local 缓存)
    bool isInLoopThread() const noexcept { return _threadId == KzThread::CurrentThread::tid(); }

    // 获取 Epoll fd
    int epollFd() const noexcept { return _poller.epollFd(); }

private:
    // 处理 wakeup fd 读事件
    void handleRead() noexcept;

    // 执行积压的任务 (Batch Processing)
    void doPendingJobs() noexcept;

    // 唤醒 Loop
    void wakeup() noexcept;

private:
    using ChannelList = Poller::ChannelList;

    // === 状态标志 ===
    std::atomic<bool> _looping{false};
    std::atomic<bool> _quit{false};
    bool _eventHandling = false;
    bool _callingPendingJobs = false;

    // 所属线程 tid
    int _threadId;

    // === 核心组件 ===
    // 声明顺序决定初始化顺序：Poller -> TimerQueue -> WakeupChannel
    Poller _poller;
    TimerQueue _timerQueue;

    // === 唤醒机制 ===
    int _wakeupFd;
    Channel _wakeupChannel;

    // 唤醒聚合标志：防止频繁 write eventfd
    // true 表示已经发出了唤醒信号，且尚未被处理
    std::atomic<bool> _isWakingUp{false};

    // === 任务队列 (双层架构) ===
    // L1: 高速无锁队列 (64K * 64B = 4MB)
    // 绝大多数情况走这里，零锁竞争，极高吞吐
    static constexpr size_t kMpscCapacity = 65536;
    KzSTL::ArrayMpscQueue<Job, kMpscCapacity> _pendingQueue;

    // L2: 溢出兜底队列 (带锁)
    // 仅在 L1 满时使用，保证绝对不丢任务
    mutable KzSTL::SpinMutex _overflowMutex;
    std::vector<Job, KzAlloc::KzAllocator<Job>> _overflowQueue;

    // L2 溢出队列缓存
    std::vector<Job, KzAlloc::KzAllocator<Job>> _l2Cache;
    size_t _l2CacheIndex = 0; // 记录当前执行到哪了

    // 活跃 Channel 缓存 (复用内存)
    ChannelList _activeChannels;

    // 默认 Poller 超时时间 (10秒)
    static constexpr int kPollTimeMs = 10000;
};

namespace detail {
inline int createEventfd() noexcept {
    // EFD_NONBLOCK | EFD_CLOEXEC
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0) [[unlikely]] {
        LOG_FATAL << "Failed in eventfd";
        abort();
    }
    return evtfd;
}
} // namespace detail

inline void EventLoop::updateChannel(Channel* channel) noexcept {
    assert(channel->ownerLoop() == this);
    assert(isInLoopThread());
    _poller.updateChannel(channel);
}

inline void EventLoop::removeChannel(Channel* channel) noexcept {
    assert(channel->ownerLoop() == this);
    assert(isInLoopThread());
    _poller.removeChannel(channel);
}

inline bool EventLoop::hasChannel(Channel* channel) noexcept {
    assert(channel->ownerLoop() == this);
    assert(isInLoopThread());
    return _poller.hasChannel(channel);
}

inline void EventLoop::quit() noexcept {
    _quit.store(true);
    // 如果在非 Loop 线程调用，必须唤醒 Loop 让它从 poll 返回
    if (!isInLoopThread()) {
        wakeup();
    }
}

inline void EventLoop::runInLoop(Job&& cb) noexcept {
    if (isInLoopThread()) {
        cb();
    }
    else {
        queueInLoop(std::move(cb));
    }
}

inline void EventLoop::queueInLoop(Job&& cb) noexcept {
    // 尝试推入 L1 无锁队列 (Fast Path)
    if (!_pendingQueue.push(std::move(cb))) [[unlikely]] {
        // L1 满了，推入 L2 溢出队列 (Slow Path)
        // 这种情况极少发生，使用锁是可接受的
        std::lock_guard<KzSTL::SpinMutex> lock(_overflowMutex);
        _overflowQueue.push_back(std::move(cb));
    }

    // 唤醒聚合 (Wakeup Coalescing)
    // 只有当：
    // 1. 不在 Loop 线程 (必须唤醒)
    // 2. 或者 正在执行 Pending Jobs (防止新任务被延迟到下一轮)
    // 且 _isWakingUp 标志为 false 时，才执行系统调用
    if (!isInLoopThread() || _callingPendingJobs) {
        bool expected = false;
        if (_isWakingUp.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            wakeup();
        }
    }
}

inline void EventLoop::wakeup() noexcept {
    uint64_t one = 1;
    ssize_t n = ::write(_wakeupFd, &one, sizeof(one));
    if (n != sizeof(one)) [[unlikely]] {
        LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
    }
}

inline void EventLoop::handleRead() noexcept {
    uint64_t one = 1;
    ssize_t n = ::read(_wakeupFd, &one, sizeof(one));
    if (n != sizeof(one)) [[unlikely]] {
        LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
    }

    // 唤醒处理完毕，重置标志
    // 这样后续的 queueInLoop 才会再次触发 wakeup
    // 使用 release 语义，保证之前的内存操作可见
    _isWakingUp.store(false, std::memory_order_release);
}

} // namespace KzNet

