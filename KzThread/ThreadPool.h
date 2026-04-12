#pragma once

#include "KzAlloc/ConcurrentAlloc.h"
#include "KzAlloc/RAII_Utils.h"
#include "KzSTL/KzString.h"
#include "KzSTL/ArrayMpmcQueue.h"
#include "KzSTL/Job.h"
#include "KzSTL/Function.h"
#include "KzSTL/WorkStealingDeque.h"
#include "KzAlgorithm/Jeaiii.h"
#include "ThreadUtils.h"

#include <string_view>
#include <vector>
#include <semaphore>
#include <new>
#include <algorithm>
#include <latch>
#include <random>
#include <span>
#include <functional>
#include <random>
#include <coroutine>
#include <thread>

using namespace std::string_view_literals;

namespace KzThread {

class ThreadPool {
#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    static constexpr size_t kCacheLineSize = 64;
#endif
public:
    enum class PoolState : uint8_t {
        STOPPED,
        STARTING,
        RUNNING,
        STOPPING
    };

public:
    static constexpr size_t kQueueCapacity = 4096;
    static constexpr size_t kLocalQueueCapacity = 1024;
    static constexpr size_t kMaxBatchSize = 64;
    static constexpr size_t kWorkerAlignment = 4096;
    using InitCallback = KzSTL::Function<void() const, 32>;
    using Job = KzSTL::Job<void(), 48>;
    using GlobalQueueType = KzSTL::ArrayMpmcQueue<Job, kQueueCapacity>;
    using GlobalQueuePtr = std::unique_ptr<GlobalQueueType, KzAlloc::AllocatorDeleter<GlobalQueueType, KzAlloc::KzAllocator<GlobalQueueType>>>;

    explicit ThreadPool(int threadNum = 8) noexcept
        : _threadNum(threadNum),
          _globalQueue(KzAlloc::allocate_unique<GlobalQueueType>()) 
    {}

    ~ThreadPool() {
        stop();
    }

    void setThreadInitCallback(InitCallback&& func) noexcept { _threadInitCallback = std::move(func); }
    void setThreadInitCallback(const InitCallback& func) noexcept { _threadInitCallback = func; }
    void setThreadNum(int threadNum) noexcept { _threadNum = threadNum; }

    void start(int first_cpu = 0, std::string_view name = "ThreadPool"sv) noexcept;
    void stop() noexcept {
        assert(t_currentWorker == nullptr
           && "FATAL: Worker thread cannot call stop() on its own pool!");

        PoolState expected = PoolState::RUNNING;
        if (_state.compare_exchange_strong(expected, PoolState::STOPPING, std::memory_order_acq_rel)) {
            // 停止所有线程
            if (_threads) {
                // 唤醒所有线程
                _globalNotifier.fetch_add(1, std::memory_order_seq_cst);
                _globalNotifier.notify_all(); 
                for (int i = 0; i < _threadNum; ++i) {
                    if (_threads[i].joinable()) {
                        _threads[i].join(); // 明确知道在这里阻塞
                    }
                    // 再调用析构函数清理裸内存对象
                    _threads[i].~thread(); 
                }
                KzAlloc::free(_threads, sizeof(std::thread) * _threadNum);
                _threads = nullptr;
            }

            // 销毁 Workers
            if (_workers) {
                for (int i = 0; i < _threadNum; ++i) {
                    _workers[i].~Worker();
                }
                KzAlloc::free_aligned(_workers, sizeof(Worker) * _threadNum, kWorkerAlignment);
                _workers = nullptr;
            }

            _state.store(PoolState::STOPPED, std::memory_order_release);
        }
    }

    size_t queueSize() noexcept {
        if (!_workers) return 0;
        size_t total = _globalQueue->sizeApprox();
        for (int i = 0; i < _threadNum; ++i) {
            total += _workers[i].local_deque.sizeApprox();
        }
        return total;
    }

    template <typename F, typename... Args>
    void run(F&& f, Args&&... args) noexcept {
        if (_state.load(std::memory_order_acquire) != PoolState::RUNNING) [[unlikely]] {
            std::forward<F>(f)(std::forward<Args>(args)...);
            return;
        }

        auto task_lambda =[func = std::forward<F>(f), 
                        ...arguments = std::forward<Args>(args)]() mutable noexcept {
            std::invoke(func, std::move(arguments)...);
        };

        dispatchJob(Job(std::move(task_lambda)));
    }

    void run(Job&& job) noexcept {
        dispatchJob(std::move(job));
    }

    void runBulk(std::span<Job> jobs) noexcept {
        if (jobs.size() == 0) [[unlikely]] return;
        // 检查状态：如果停止，当前线程执行所有任务
        if (_state.load(std::memory_order_acquire) != PoolState::RUNNING) [[unlikely]] {
            for (auto& job : jobs) { job(); }
            return;
        }
        
        size_t jobs_processed = 0;
        // 如果是 Worker 线程自己提交的子任务，优先放入本地队列
        if (t_currentWorker != nullptr && t_currentWorker->pool == this) {
            if (jobs_processed += t_currentWorker->local_deque.pushBulk(jobs)) {
                // 插入全屏障，确保 enqueue 的写入对其他核心可见，
                // 且严格发生在 load sleepingWorkerCount 之前。
                std::atomic_thread_fence(std::memory_order_seq_cst); 
                // 本地队列 push 成功，如果有其他线程在睡觉，唤醒一个来偷
                if (_sleepingWorkerCount.load(std::memory_order_relaxed) > 0) {
                    wakeUpOneWorker();
                }
                if (jobs_processed == jobs.size()) return;
            }
        }

        size_t pushed_global = _globalQueue->enqueueBulk(jobs.subspan(jobs_processed));
        if (pushed_global > 0) {
            jobs_processed += pushed_global;
            std::atomic_thread_fence(std::memory_order_seq_cst); 
            if (_sleepingWorkerCount.load(std::memory_order_relaxed) > 0) {
                wakeUpOneWorker();
            }
            if (jobs_processed == jobs.size()) return;
        }

        for (auto& job : jobs.subspan(jobs_processed)) {
            job();
        }
    }

private:

    void wakeUpOneWorker() noexcept {
        _globalNotifier.fetch_add(1, std::memory_order_release);
        _globalNotifier.notify_one();
    }

    void dispatchJob(Job&& job) noexcept {
        // 如果是 Worker 线程自己提交的子任务，优先放入本地队列
        if (t_currentWorker != nullptr && t_currentWorker->pool == this) {
            if (t_currentWorker->local_deque.push(std::move(job))) {
                std::atomic_thread_fence(std::memory_order_seq_cst); 
                // 本地队列 push 成功，如果有其他线程在睡觉，唤醒一个来偷
                if (_sleepingWorkerCount.load(std::memory_order_relaxed) > 0) {
                    wakeUpOneWorker();
                }
                return;
            }
        }

        // 外部线程提交，或者本地队列已满，推入全局队列
        if (_globalQueue->enqueue(std::move(job))) {
            std::atomic_thread_fence(std::memory_order_seq_cst); 
            if (_sleepingWorkerCount.load(std::memory_order_relaxed) > 0) {
                wakeUpOneWorker();
            }
            return;
        }

        // 极端情况：全局队列也满了
        job(); // Caller-Runs
    }

    struct alignas(kWorkerAlignment) Worker {
        int id = 0;
        ThreadPool* pool = nullptr;
        KzSTL::WorkStealingDeque<Job, kLocalQueueCapacity> local_deque; 

        Worker() noexcept = default;
    };
    void workerLoop(Worker* myWorker) noexcept;

private:
    // 用于记录当前有多少个线程在睡眠，优化唤醒逻辑
    alignas(kCacheLineSize) std::atomic<uint64_t> _sleepingWorkerCount{0};

    alignas(kCacheLineSize) std::atomic<uint64_t> _globalNotifier{0};
    std::atomic<PoolState> _state{PoolState::STOPPED};

    int _threadNum;

    std::thread* _threads = nullptr;
    Worker* _workers = nullptr;

    // 全局 MPMC 队列
    GlobalQueuePtr _globalQueue;
    
    InitCallback _threadInitCallback;

    // 用于识别当前线程是否是 Worker
    inline static thread_local Worker* t_currentWorker = nullptr; 
};


inline void ThreadPool::start(int first_cpu, std::string_view name) noexcept {
    PoolState expected = PoolState::STOPPED;
    if (!_state.compare_exchange_strong(expected, PoolState::STARTING, std::memory_order_acquire)) return;

    // 分配 Worker 内存
    void* workerMemory = KzAlloc::malloc_aligned(sizeof(Worker) * _threadNum, kWorkerAlignment);
    if (!workerMemory) [[unlikely]] KzAlloc::handleOOM();
    _workers = static_cast<Worker*>(workerMemory);

    // 分配 Thread 内存
    void* threadMemory = KzAlloc::malloc(sizeof(std::thread) * _threadNum);
    if (!threadMemory) [[unlikely]] KzAlloc::handleOOM();
    _threads = static_cast<std::thread*>(threadMemory);

    // 不需要 shared_ptr，直接在栈上分配 latch
    // 因为 start() 会阻塞等待 latch 归零，所以局部变量绝对安全
    std::latch initLatch(_threadNum);

   // try {
        for (int i = first_cpu; i < _threadNum + first_cpu; ++i) {
            char idBuf[128];
            size_t nameLen = std::min(name.size(), static_cast<size_t>(108));
            ::memcpy(idBuf, name.data(), nameLen);
            char* end = KzAlgorithm::Jeaiii::to_chars(idBuf + nameLen, static_cast<uint64_t>(i));

            Worker* workerAddr = &_workers[i];

            new (&_threads[i]) std::thread(
                [this, i, workerAddr, name = KzSTL::KzString(idBuf, end - idBuf),
                    cb = this->_threadInitCallback, &initLatch]() mutable noexcept {

                    new (workerAddr) Worker();
                    workerAddr->id = i;
                    workerAddr->pool = this;
            
                    unsigned int hw_threads = std::thread::hardware_concurrency();
                    size_t coreId = i % (hw_threads == 0 ? 1 : hw_threads);
                    ThreadUtils::set_thread_name(std::string_view(name));
                    ThreadUtils::pin_to_core(coreId);
                    ThreadUtils::enable_local_memory_policy();

                    if (cb) {
                        cb();
                    }

                    initLatch.count_down();

                     while (_state.load(std::memory_order_acquire) != PoolState::RUNNING) {
                         #if defined(_MSC_VER)
                        _mm_pause();
                        #elif defined(__GNUC__) || defined(__clang__)
                        __builtin_ia32_pause();
                        #endif 
                    }
                    this->workerLoop(workerAddr);
                }
            );
        }
   // }
   // catch(...) {
     //   LOG_FATAL << "Failed to start thread pool"; // 直接退出程序
   // }

    initLatch.wait();
    _state.store(PoolState::RUNNING, std::memory_order_release);
}


inline void ThreadPool::workerLoop(Worker* myWorker) noexcept {
    // 绑定 thread_local 指针，让 run() 知道我们是内部线程
    t_currentWorker = myWorker; 
    
    std::array<Job, kMaxBatchSize> localBatch;
    static thread_local std::mt19937 rng(std::random_device{}());

    while (_state.load(std::memory_order_relaxed) == PoolState::RUNNING) {
        Job task;

        // 优先级 1：本地 Chase-Lev 队列 (无锁 LIFO)
        if (myWorker->local_deque.pop(task)) {
            task();
            task = Job(); // 显式析构
            continue;
        }

        // 优先级 2：全局 MPMC 队列进货 (批量 FIFO)
        size_t grabbed = _globalQueue->dequeueBulk(localBatch.begin(), kMaxBatchSize);
        if (grabbed > 0) {
            if (grabbed > 1) {
                myWorker->local_deque.pushBulk(std::span{localBatch.begin() + 1, grabbed - 1});
                // 叫醒一个线程来偷任务
                if (_sleepingWorkerCount.load(std::memory_order_relaxed) > 0) {
                    wakeUpOneWorker();
                }
            }
            localBatch[0]();
            localBatch[0] = Job();
            continue;
        }   

        // 优先级 3：窃取其他 Worker 的任务 (FIFO)
        bool stolen = false;
        int victimIdx = rng() % _threadNum;
        for (int i = 0; i < _threadNum; ++i) {
            if (victimIdx != myWorker->id) {
                Worker* victim = &_workers[victimIdx];
                if (victim->local_deque.steal(task)) {
                    stolen = true;
                    break;
                }
            }
            if (++victimIdx == _threadNum) victimIdx = 0;
        }

        if (stolen) {
            task();
            task = Job();
            continue;
        }

        // 优先级 4：短暂自旋
        bool hasWork = false;
        for (int i = 0; i < 256; ++i) {
            if (_globalQueue->sizeApprox() > 0) {
                if (_globalQueue->dequeue(task)) {
                hasWork = true;
                break;
            }
        }
            #if defined(_MSC_VER)
            _mm_pause();
            #elif defined(__GNUC__) || defined(__clang__)
            __builtin_ia32_pause();
            #endif
        }
        if (hasWork) {
            task();
            task = Job();
            continue;
        }

        // 优先级 5：EventCount 深度睡眠
        // 确保 sleepingCount 的增加对 Producer 可见，再检查队列
        _sleepingWorkerCount.fetch_add(1, std::memory_order_seq_cst);
        
        uint64_t ticket = _globalNotifier.load(std::memory_order_acquire);
        
        // 在拿到 ticket 之后，真正 wait 之前，必须检查线程池是否已经停机
        // 如果此时 _state != RUNNING，说明主线程已经发出了停机信号
        // 这里用于防御在该线程拿到 ticket 前 stop 已经执行，那么 wait 时 ticket 对应的_globalNotifier 就不会变
        // 就导致 stop 唤醒后依然睡眠
        if (_state.load(std::memory_order_acquire) != PoolState::RUNNING) [[unlikely]] {
            _sleepingWorkerCount.fetch_sub(1, std::memory_order_seq_cst);
            break; // 直接退出 Worker 循环
        }

        // 睡前必须同时检查全局和所有局部队列
        if (_globalQueue->sizeApprox() > 0) {
            hasWork = true;
        } 
        else {
            for (int i = 0; i < _threadNum; ++i) {
                if (_workers[i].local_deque.sizeApprox() > 0) {
                    hasWork = true;
                    break;
                }
            }
        }
        if (!hasWork) {
            _globalNotifier.wait(ticket, std::memory_order_relaxed);
        }
        
        _sleepingWorkerCount.fetch_sub(1, std::memory_order_relaxed);
    }

    // 线程退出前清理 thread_local
    t_currentWorker = nullptr;
}

struct ResumeOnThreadPool {
    ThreadPool* _pool;

    explicit ResumeOnThreadPool(ThreadPool* pool) : _pool(pool) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) const {
        // 将协程恢复任务投递到线程池
        _pool->run([h]() mutable noexcept {
            h.resume();
        });
    }

    void await_resume() const noexcept {}
};

} // namespace KzThread