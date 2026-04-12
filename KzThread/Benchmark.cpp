#include "ThreadPool.h"
#include <iostream>
#include <chrono>
#include <atomic>
#include <vector>
#include <iomanip>
#include <thread>
#include <emmintrin.h>

using namespace KzThread;
using namespace std::chrono;

// --- 辅助工具：防止编译器优化掉空循环 ---
inline void doNotOptimize(int value) {
#if defined(__clang__) || defined(__GNUC__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    // MSVC fallback
    volatile int dummy = value;
#endif
}

// --- 模拟真实工作负载 ---
void dummyWork(int iterations) {
    int sum = 0;
    for (int i = 0; i < iterations; ++i) {
        sum += i * 13;
        doNotOptimize(sum);
    }
}

// --- 格式化输出工具 ---
void printResult(const std::string& name, size_t tasks, double ms) {
    double mops = (tasks / 1000000.0) / (ms / 1000.0);
    std::cout << std::left << std::setw(30) << name 
              << std::right << std::setw(15) << tasks 
              << std::setw(15) << std::fixed << std::setprecision(2) << ms << " ms"
              << std::setw(15) << std::fixed << std::setprecision(2) << mops << " Mops/s\n";
}

// ============================================================================
// 测试场景 1：SPMC (单生产者，多消费者) - 纯调度开销
// ============================================================================
void bench_SPMC_Light(ThreadPool& pool, size_t numTasks) {
    alignas(64) std::atomic<size_t> counter{0};
    
    auto start = high_resolution_clock::now();
    
    for (size_t i = 0; i < numTasks; ++i) {
        pool.run([&counter]() noexcept {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    // 阻塞等待所有任务完成
    while (counter.load(std::memory_order_acquire) < numTasks) {
        #if defined(_MSC_VER)
        _mm_pause();
        #elif defined(__GNUC__) || defined(__clang__)
        __builtin_ia32_pause();
        #endif 
    }
    
    auto end = high_resolution_clock::now();
    printResult("1. SPMC (Light Tasks)", numTasks, duration<double, std::milli>(end - start).count());
}

// ============================================================================
// 测试场景 2：Bulk Enqueue (批量提交) - 测试极速路径
// ============================================================================
void bench_Bulk_Light(ThreadPool& pool, size_t numTasks) {
    alignas(64) std::atomic<size_t> counter{0};
    std::vector<ThreadPool::Job> jobs;
    jobs.reserve(numTasks);
    
    for (size_t i = 0; i < numTasks; ++i) {
        jobs.emplace_back([&counter]() noexcept {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    auto start = high_resolution_clock::now();
    
    // 每次批量提交 64 个任务 (模拟网络库中一次 epoll 唤醒处理多个 fd)
    constexpr size_t batchSize = 64;
    for (size_t i = 0; i < numTasks; i += batchSize) {
        size_t endIdx = std::min(i + batchSize, numTasks);
        pool.runBulk(std::span<ThreadPool::Job>{jobs.data() + i, endIdx - i});
    }
    
    while (counter.load(std::memory_order_acquire) < numTasks) {
        #if defined(_MSC_VER)
        _mm_pause();
        #elif defined(__GNUC__) || defined(__clang__)
        __builtin_ia32_pause();
        #endif 
    }
    
    auto end = high_resolution_clock::now();
    printResult("2. Bulk Enqueue (Batch=64)", numTasks, duration<double, std::milli>(end - start).count());
}

// ============================================================================
// 测试场景 3：Nested Tasks (嵌套任务) - 拷问 Chase-Lev 队列与 Work-Stealing
// ============================================================================
void bench_Nested_Stealing(ThreadPool& pool, size_t rootTasks, size_t subTasks) {
    alignas(64) std::atomic<size_t> counter{0};
    size_t totalTasks = rootTasks * subTasks;
    
    auto start = high_resolution_clock::now();
    
    for (size_t i = 0; i < rootTasks; ++i) {
        pool.run([&pool, &counter, subTasks]() noexcept {
            // Worker 线程内部疯狂提交子任务，这些任务会全部进入本地 Chase-Lev 队列
            for (size_t j = 0; j < subTasks; ++j) {
                pool.run([&counter]() noexcept {
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    
    while (counter.load(std::memory_order_acquire) < totalTasks) {
        #if defined(_MSC_VER)
        _mm_pause();
        #elif defined(__GNUC__) || defined(__clang__)
        __builtin_ia32_pause();
        #endif 
    }
    
    auto end = high_resolution_clock::now();
    printResult("3. Nested (Work Stealing)", totalTasks, duration<double, std::milli>(end - start).count());
}

// ============================================================================
// 测试场景 4：Compute Heavy (计算密集) - 测试负载均衡
// ============================================================================
void bench_Compute_Heavy(ThreadPool& pool, size_t numTasks) {
    alignas(64) std::atomic<size_t> counter{0};
    
    auto start = high_resolution_clock::now();
    
    for (size_t i = 0; i < numTasks; ++i) {
        pool.run([&counter]() noexcept {
            dummyWork(1000); // 模拟耗时计算
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    while (counter.load(std::memory_order_acquire) < numTasks) {
        #if defined(_MSC_VER)
        _mm_pause();
        #elif defined(__GNUC__) || defined(__clang__)
        __builtin_ia32_pause();
        #endif 
    }
    
    auto end = high_resolution_clock::now();
    printResult("4. Compute Heavy (1k iters)", numTasks, duration<double, std::milli>(end - start).count());
}

int main() {
    int hwThreads = std::thread::hardware_concurrency();
    // int poolThreads = hwThreads > 0 ? hwThreads : 4;
    int poolThreads = 4;
    std::cout << "==============================================================\n";
    std::cout << " KzThread::ThreadPool Benchmark\n";
    std::cout << " Hardware Threads: " << hwThreads << "\n";
    std::cout << " Pool Threads:     " << poolThreads << "\n";
    std::cout << "==============================================================\n";
    std::cout << std::left << std::setw(30) << "Scenario" 
              << std::right << std::setw(15) << "Tasks" 
              << std::setw(18) << "Time" 
              << std::setw(18) << "Throughput\n";
    std::cout << "--------------------------------------------------------------\n";

    ThreadUtils::pin_to_core(4);
    ThreadUtils::enable_local_memory_policy();
    // 初始化线程池
    ThreadPool pool(poolThreads);
    pool.start(0, "BenchWorker");

    // 预热 (Warm-up) - 让 OS 分配好物理页，让 CPU 缓存热起来
    bench_SPMC_Light(pool, 100'000);
    std::cout << "--- Warm-up completed ---\n";

    // 正式测试
    constexpr size_t kLightTasks = 5'000'000; // 500万轻量任务
    
    bench_SPMC_Light(pool, kLightTasks);
    bench_Bulk_Light(pool, kLightTasks);
    
    // 1000 个根任务，每个根任务提交 5000 个子任务 = 500万任务
    bench_Nested_Stealing(pool, 1000, 5000); 
    
    // 50万个重度计算任务
    bench_Compute_Heavy(pool, 500'000);

    std::cout << "==============================================================\n";
    
    pool.stop();
    return 0;
}