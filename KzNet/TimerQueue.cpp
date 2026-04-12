#include "TimerQueue.h"

#include "EventLoop.h"

using namespace KzTimer;
namespace KzNet {
using TimerIdVector = std::vector<TimerId, KzAlloc::KzAllocator<TimerId>>;
using TimerReqVector = std::vector<TimerRequest, KzAlloc::KzAllocator<TimerRequest>>;

void TimerQueue::advanceTo(TimeStamp now) noexcept {
    assert(_loop->isInLoopThread());
    
    int64_t nowNs = now.nanoseconds();
    // 计算从启动到现在，物理时间应该走过多少个 Tick
    int64_t targetTicks = (nowNs - _startTimeNs) / kTickNs;

    // 追赶落后的 Ticks
    // 如果 epoll_wait 睡了 30ms，这里就会循环 3 次，依次处理错过的槽位
    while (_currentTicks < targetTicks) {
        ++_currentTicks;
        processExpiredTimers();
    }
}

// === 核心逻辑 ===
TimerId TimerQueue::addTimerInLoop(Job&& cb,
    TimeStamp when, int64_t interval, bool strict) noexcept {
    // 仅限 IO 线程调用
    assert(_loop->isInLoopThread());
    void* mem = KzAlloc::malloc(sizeof(Timer));
    if (!mem) [[unlikely]] KzAlloc::handleOOM();
    Timer* timer = new (mem) Timer(std::move(cb), when, interval, strict);
    return addTimerInLoop(timer);
}

void TimerQueue::addTimerDetached(Job&& cb,
    TimeStamp when, int64_t interval, bool strict) noexcept {
    if (_loop->isInLoopThread()) {
        void* mem = KzAlloc::malloc(sizeof(Timer));
        if (!mem) [[unlikely]] KzAlloc::handleOOM();
        auto* timer = new (mem) Timer(std::move(cb), when, interval, strict);
        addTimerDetachedInLoop(timer);
    } else {
        _loop->runInLoop([this, cb = std::move(cb), when, interval, strict]() mutable noexcept {
            void* mem = KzAlloc::malloc(sizeof(Timer));
            if (!mem) [[unlikely]] KzAlloc::handleOOM();
            auto* timer = new (mem) Timer(std::move(cb), when, interval, strict);
            this->addTimerDetachedInLoop(timer);
        });
    }
}

TimerId TimerQueue::addTimerInLoop(Timer* timer) noexcept {
    assert(this->_loop->isInLoopThread());
    // 分配句柄
    int32_t idx = allocSlot(timer);
    timer->_handleIndex = idx;

    // 计算时间轮位置
    int64_t expNs = timer->_expiration.nanoseconds();
    int64_t targetTicks = (expNs - _startTimeNs) / kTickNs;
    int64_t ticks = targetTicks - _currentTicks;
    // 强制至少延迟 1 个 Tick，防止时间黑洞
    /* 时间黑洞推演：
    假设当前时间轮正在执行 processExpiredTimers()，此时 _currentTicks = 100。
    在执行某个定时器回调时，用户又添加了一个**立即执行（delay = 0）**的定时器。
    按照逻辑，ticks = 0，这个新定时器被挂到了 _wheels[100] 这个槽位里。
    但是当前的 _wheels[100] 已经被 std::move 拿走并正在遍历执行了。这个新加的定时器会被留在原本的空槽位里。
    时间轮的指针 _currentTicks 会继续往前走（101, 102...）。
    结果： 这个本该立即执行的定时器，必须等待时间轮转完整整一圈（4096 个 Tick，即 4.096 秒）后，才能再次被触发。
    */
    if (ticks <= 0) ticks = 1;
    timer->_rotationCount = ticks / kWheelSize;
    timer->_slotIndex = (_currentTicks + ticks) & kWheelMask;

    // 插入
    _wheels[timer->_slotIndex].push_back(timer);
    ++_timerCount;

    return TimerId(idx, timer->_sequence);
}

void TimerQueue::addTimerDetachedInLoop(Timer* timer) noexcept {
    assert(_loop->isInLoopThread());
    // 不需要分配句柄
    timer->_handleIndex = -1;

    // 计算时间轮位置
    int64_t expNs = timer->_expiration.nanoseconds();
    int64_t targetTicks = (expNs - _startTimeNs) / kTickNs;
    int64_t ticks = targetTicks - _currentTicks;
    
    if (ticks <= 0) ticks = 1;
    timer->_rotationCount = ticks / kWheelSize;
    timer->_slotIndex = (_currentTicks + ticks) & kWheelMask;

    // 插入
    _wheels[timer->_slotIndex].push_back(timer);
    ++_timerCount;
}

void TimerQueue::cancel(TimerId timerId) noexcept {
    // 线程安全代理
    if (!_loop->isInLoopThread()) {
        _loop->runInLoop([this, id = timerId] mutable noexcept { this->cancel(id); });
        return;
    }

    int32_t idx = timerId._index;
    // 安全校验
    if (idx < 0 || idx >= static_cast<int32_t>(_handles.size())
        || timerId._sequence != _handles[idx]._sequence) [[unlikely]] return;
    _handles[idx]._timer->_canceled = true;  // 惰性取消
}

void TimerQueue::cancel(const TimerIdVector& timerIds) noexcept {
    // 线程安全代理
    if (!_loop->isInLoopThread()) {
        _loop->runInLoop([this, ids = timerIds] mutable noexcept { 
            for (auto id : ids) this->cancel(id);
        });
        return;
    }

    for(size_t i = 0; i < timerIds.size(); ++i) {
        int32_t idx = timerIds[i]._index;
        // 安全校验
        if (idx < 0 || idx >= static_cast<int32_t>(_handles.size())
            || timerIds[i]._sequence != _handles[idx]._sequence) [[unlikely]] continue;
        _handles[idx]._timer->_canceled = true;  // 惰性取消
    }
}

void TimerQueue::processExpiredTimers() noexcept {
    int slot = _currentTicks & kWheelMask;
    auto& list = _wheels[slot];

    // 直接移动接管链表，避免逐个 pop
    TimerList pendingList = std::move(list);

    // 这个批次里的所有定时器，都共享这一个“触发时刻”
    // 这里使用 TimeStamp now(_startTimeNs + _currentTicks * kTickNs) 
    // 也就是当前时间 = 启动时间 + 走过的 Tick 数 * 每个 Tick 的时长
    // 因为实际上 OS 调度会有微小延迟，所以不使用 TimeStamp::now()
    TimeStamp logicalNow(_startTimeNs + _currentTicks * kTickNs);
    while (Timer* timer = pendingList.pop_front()) {
        // 检查取消
        if (timer->_canceled) {
            _timerCount--;
            freeTimer(timer);
            continue;
        }

        // 检查轮次
        if (timer->_rotationCount > 0) {
            timer->_rotationCount--;
            list.push_back(timer); // 放回当前槽位
            continue;
        }

        // 执行
        timer->run(); // 契约，不允许抛异常

        // 防止回调内部取消了自己，却又被当成 repeat 重新插入
        if (timer->_canceled) {
            _timerCount--;
            freeTimer(timer);
            continue;
        }

        // 重复任务
        if (timer->_repeat) {
            timer->restart(logicalNow);

            // 基于绝对时间锚点重新计算插入
            int64_t expNs = timer->_expiration.nanoseconds();
            int64_t targetTicks = (expNs - _startTimeNs) / kTickNs;
            int64_t ticks = targetTicks - _currentTicks;
            
            if (ticks <= 0) ticks = 1;
            timer->_rotationCount = ticks / kWheelSize;
            timer->_slotIndex = (_currentTicks + ticks) & kWheelMask;

            _wheels[timer->_slotIndex].push_back(timer);
        }
        else {
            _timerCount--;
            freeTimer(timer);
        }
    }
}

// Awaiter 实现
// 单个添加的 Awaiter
struct TimerQueue::AddTimerAwaiter {
    TimerQueue* self;
    KzThread::ThreadPool* pool;
    Job cb;
    TimeStamp when;
    int64_t interval;
    bool strict;
    TimerId resultId;

    bool await_ready() { 
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        self->_loop->runInLoop([this, h]() mutable noexcept {
            this->resultId = self->addTimerInLoop(std::move(cb), when, interval, strict);
            if (this->pool) [[likely]] {
                this->pool->run([h]() mutable noexcept { h.resume(); });
            } else {
                h.resume();
            }
        });
    }

    TimerId await_resume() {
        return resultId;
    }
};

KzSTL::Task<TimerId> TimerQueue::addTimerAsync(KzThread::ThreadPool* pool,
    Job&& cb, TimeStamp when, int64_t interval, bool strict) noexcept {
    co_return co_await AddTimerAwaiter{this, pool, std::move(cb), when, interval, strict};
}

// 批量添加的 Awaiter
struct TimerQueue::AddTimersAwaiter {
    TimerQueue* self;
    KzThread::ThreadPool* pool;
    TimerReqVector reqs;
    TimerIdVector resultIds;
    
    AddTimersAwaiter(TimerQueue* q, KzThread::ThreadPool* p, TimerReqVector&& requests) 
        : self(q), pool(p), reqs(std::move(requests))
    {}

    bool await_ready() { 
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        self->_loop->runInLoop([this, h]() mutable noexcept {
            this->resultIds = self->addTimersInLoop(std::move(this->reqs));
            if (this->pool) [[likely]] {
                this->pool->run([h]() mutable noexcept { h.resume(); });
            } else {
                h.resume();
            }
        });
    }

    TimerIdVector await_resume() {
        return std::move(resultIds);
    }
};

KzSTL::Task<TimerIdVector> TimerQueue::addTimersAsync(KzThread::ThreadPool* pool,
    TimerReqVector&& reqs) noexcept {
    co_return co_await AddTimersAwaiter{this, pool, std::move(reqs)};
}

// 内部批量插入实现
TimerIdVector TimerQueue::addTimersInLoop(TimerReqVector&& reqs) noexcept {
    assert(_loop->isInLoopThread());
    TimerIdVector resultIds;
    resultIds.reserve(reqs.size());

    for (auto& req : reqs) {
        void* mem = KzAlloc::malloc(sizeof(Timer));
        if (!mem) [[unlikely]] KzAlloc::handleOOM();
        Timer* timer = new (mem) Timer(std::move(req.cb), req.when, req.intervalNs, req.isStrict);
        resultIds.push_back(addTimerInLoop(timer));
    }
    return resultIds;
}

void TimerQueue::addTimersDetached(TimerReqVector&& reqs) noexcept {
    if (_loop->isInLoopThread()) {
        addTimersDetachedInLoop(std::move(reqs));
        return;
    }

    _loop->runInLoop([this, reqs = std::move(reqs)]() mutable noexcept {
        this->addTimersDetachedInLoop(std::move(reqs));
    });
}

// 内部批量插入实现(无返回)
void TimerQueue::addTimersDetachedInLoop(TimerReqVector&& reqs) noexcept {
    assert(_loop->isInLoopThread());

    for (auto& req : reqs) {
        void* mem = KzAlloc::malloc(sizeof(Timer));
        if (!mem) [[unlikely]] KzAlloc::handleOOM();
        Timer* timer = new (mem) Timer(std::move(req.cb), req.when, req.intervalNs, req.isStrict);
        addTimerDetachedInLoop(timer);
    }
}

} // namespace KzTimer