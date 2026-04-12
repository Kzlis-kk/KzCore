#include "EventLoop.h"
#include "KzLog/Logger.h"

#include <unistd.h>
#include <signal.h>
#include <algorithm>

namespace KzNet {
thread_local EventLoop* t_loopInThisThread = nullptr;

using TimerIdVector = std::vector<KzTimer::TimerId, KzAlloc::KzAllocator<KzTimer::TimerId>>;
using TimerReqVector = std::vector<KzTimer::TimerRequest, KzAlloc::KzAllocator<KzTimer::TimerRequest>>;

// RAII保护 _eventHandling 和 _callingPendingJobs
struct BoolGuard {
    bool& flag;
    BoolGuard(bool& f) : flag(f) { flag = true; }
    ~BoolGuard() { flag = false; }
};

// 忽略 SIGPIPE
__attribute__((constructor(101))) 
    void initIgnoreSigPipe() {
        ::signal(SIGPIPE, SIG_IGN);
    }

EventLoop::EventLoop() noexcept
    : _threadId(KzThread::CurrentThread::tid()),
      _poller(this),
      _timerQueue(this),
      _wakeupFd(detail::createEventfd()),
      _wakeupChannel(this, _wakeupFd)
{

    if (t_loopInThisThread) [[unlikely]] {
        LOG_FATAL << "Another EventLoop " << t_loopInThisThread 
                  << " exists in this thread " << _threadId;
    }
    else {
        t_loopInThisThread = this;
    }

    // 设置 wakeup channel
    _wakeupChannel.setReadCallback([this](KzTimer::TimeStamp) mutable noexcept { handleRead(); });
    _wakeupChannel.enableReading();

    // 预留空间
    _activeChannels.reserve(64);
    // _overflowQueue 默认空，按需增长
}

EventLoop::~EventLoop() noexcept {
        _wakeupChannel.disableAll();
        _wakeupChannel.remove();
        
        if (_wakeupFd) [[likely]] ::close(_wakeupFd);

        t_loopInThisThread = nullptr;
    }

void EventLoop::loop() noexcept {
    assert(!_looping);
    assert(isInLoopThread());

    _looping.store(true, std::memory_order_release);
    _quit.store(false, std::memory_order_release);

    while (!_quit.load(std::memory_order_acquire)) {
        _activeChannels.clear();

        // 如果有定时器，最多睡 10ms (kTickNs)；如果没有，最多睡 kPollTimeMs
        int timeoutMs = _timerQueue.hasTimers() ? 10 : kPollTimeMs;
        
        _poller.poll(timeoutMs, &_activeChannels);

        // 立刻获取当前时间
        KzTimer::TimeStamp cachedTime = KzTimer::TimeStamp::now();

        // Handle IO Events
        {
            BoolGuard eventGuard(_eventHandling);
            for (auto channel : _activeChannels) {
                // 如果某个 channel 执行回调的时候删除后把即将要处理的 channel 断开了
                // 此时要求上层的断开逻辑必须是 queueInLoop 延迟销毁，不然会 UAF，这是个契约
                channel->handleEvent(KzTimer::TimeStamp::now());
            }
        }

        if (_timerQueue.hasTimers()) {
            _timerQueue.advanceTo(cachedTime);
        }

        // Handle Pending Functors
        // 必须在 IO 事件处理完后立即执行，保证低延迟
        doPendingJobs();
    }

    _looping.store(false, std::memory_order_release);
    // 即使 quit 为 true 跳出了循环，也必须把队列里剩余的任务（特别是连接销毁任务）执行完
    // 循环执行，直到 L1 和 L2 队列被彻底排空
    bool hasMore = true;
    while (hasMore) {
        doPendingJobs();
        std::lock_guard<KzSTL::SpinMutex> lock(_overflowMutex);
        hasMore = (_pendingQueue.sizeApprox() > 0) || (!_overflowQueue.empty()) || (_l2CacheIndex < _l2Cache.size());
    }
    
}

void EventLoop::doPendingJobs() noexcept {
    BoolGuard pendingGuard(_callingPendingJobs);

    // 栈上批量处理 (Stack Batching)
    // 1024 个 Job * 64 bytes = 64KB，L1/L2 Cache 友好
    static constexpr size_t kBatchSize = 1024;
    std::array<Job, kBatchSize> jobs;

    // 限制单次 Loop 处理的任务总数，防止 IO 饿死
    // 这里限制处理 65536 个任务
    static constexpr int kMaxTasks = 65536;
    int processedCount = 0;

    while (processedCount < kMaxTasks) {
        // 1. 优先处理 L1 无锁队列
        size_t count = _pendingQueue.popBulk(jobs);

        if (count > 0) {
            for (size_t i = 0; i < count; ++i) {
                jobs[i]();
                // 立刻析构，确保严密的生命周期以及保证了jobs作为复用存储其中每个job在被覆盖前一定会析构一次
                jobs[i] = nullptr;
            }
            processedCount += count;
            // 如果 L1 还有货，优先处理 L1，保持热度
            if (processedCount < kMaxTasks) continue; 
        }

        // 2. 如果 L1 空了，检查 L2 溢出队列
        // 2.1 如果缓存空了，去 L2 进货
        if (_l2CacheIndex >= _l2Cache.size()) {
            _l2Cache.clear(); // 清空旧数据 (不释放内存，保留 capacity)
            _l2CacheIndex = 0;

            std::lock_guard<KzSTL::SpinMutex> lock(_overflowMutex);
            if (_overflowQueue.empty()) {
                // L1 没货，L2 也没货，彻底没事做了
                break;
            }
            // O(1) 交换
            _l2Cache.swap(_overflowQueue);
        }

        // 2.2 执行缓存里的任务
        // 注意：这里不需要一次性执行完，受 kMaxTasks 限制
        size_t toProcess = std::min(_l2Cache.size() - _l2CacheIndex, static_cast<size_t>(kMaxTasks - processedCount));

        for (size_t i = 0; i < toProcess; ++i) {
            _l2Cache[_l2CacheIndex]();
            // 及时释放资源 (对于持有大内存的闭包很重要)
            _l2Cache[_l2CacheIndex] = nullptr; 
            _l2CacheIndex++;
        }
        
        processedCount += toProcess;
    }
    // 退出逻辑：
    // 如果是因为达到 kMaxTasks 限制而退出的，说明可能还有任务没做完
    // (无论是 L1 里还有，还是 _l2Cache 里还有)
    // 必须唤醒自己，确保下一轮 Loop 立刻继续处理，而不是阻塞在 epoll_wait
    if ((_pendingQueue.sizeApprox() > 0) || 
            (_l2CacheIndex < _l2Cache.size())) {
        bool expected = false;
        if (_isWakingUp.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            wakeup();
        }
    }
}

void EventLoop::queueInLoopBatch(std::span<Job> funcs) noexcept {
    size_t count = funcs.size();
    // 尝试推入 L1 (无锁批量)
    // enqueueBulk 返回实际入队的数量
    size_t funcs_processed = _pendingQueue.pushBulk(funcs);

    // 如果没塞完 (L1 满了)，剩余的推入 L2 (带锁)
    if (funcs_processed < count) [[unlikely]] {
        // 推进迭代器到未处理的位置
        
        std::lock_guard<KzSTL::SpinMutex> lock(_overflowMutex);
        // 预留空间
        if (_overflowQueue.capacity() < _overflowQueue.size() + (count - funcs_processed)) {
            _overflowQueue.reserve(_overflowQueue.size() + (count - funcs_processed));
        }
        // 批量搬运
        for (auto& func : funcs.subspan(funcs_processed)) {
                _overflowQueue.push_back(std::move(func));
            }
    }

    // 唤醒聚合
    if (!isInLoopThread() || _callingPendingJobs) {
        bool expected = false;
        if (_isWakingUp.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            wakeup();
        }
    }
}

} // namespace KzNet