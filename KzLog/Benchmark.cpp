#include "Logger.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <string>
#include "KzSTL/KzString.h"
// #include "KzThread/ThreadUtils.h"

using namespace std::chrono;
using namespace std::string_view_literals;

// ============================================================================
// 测试参数配置
// ============================================================================
const int kMessagesPerThread = 1'000'000; // 每个线程写 100 万条日志
const std::string kLongString(500, 'X');  // 用于模拟大日志的 500 字节长字符串

// 发令枪：确保所有线程同时开始，制造最大并发压力
std::atomic<int> g_ready_threads{0};
std::atomic<bool> g_start_flag{false};

// ============================================================================
// 核心测试逻辑
// ============================================================================
void BenchThread(int thread_id, bool is_long_msg, double& out_avg_latency_ns) {

    // const unsigned int num_cores = std::thread::hardware_concurrency();
    // KzThread::ThreadUtils::pin_to_core(thread_id % num_cores);
    // KzThread::ThreadUtils::enable_local_memory_policy();

    // 1. 准备阶段：等待发令枪
    g_ready_threads.fetch_add(1, std::memory_order_relaxed);
    while (!g_start_flag.load(std::memory_order_acquire)) {
        // 自旋等待，确保所有线程的 CPU 缓存处于热状态
    }

    // 2. 计时开始
    auto start_time = high_resolution_clock::now();

    // 3. 疯狂写入
    if (is_long_msg) {
        for (int i = 0; i < kMessagesPerThread; ++i) {
            LOG_INFO << "Thread " << thread_id << " writing long message id: " << i 
                     << " payload: " << kLongString;
        }
    } else {
        for (int i = 0; i < kMessagesPerThread; ++i) {
            LOG_INFO << "Thread " << thread_id << " writing short message id: " << i;
        }
    }

    // 4. 计时结束
    auto end_time = high_resolution_clock::now();
    auto duration_ns = duration_cast<nanoseconds>(end_time - start_time).count();

    // 5. 计算前端平均延迟 (纳秒/条)
    out_avg_latency_ns = static_cast<double>(duration_ns) / kMessagesPerThread;
}

void RunBenchmark(int num_threads, bool is_long_msg, std::string_view log_path) noexcept {
    std::cout << "==================================================\n";
    std::cout << "Threads: " << std::setw(2) << num_threads 
              << " | Msg Type: " << (is_long_msg ? "Long (~550B)" : "Short (~50B)") << "\n";
    
    // 初始化日志引擎
    KzLog::InitAsyncLogger(log_path);

    std::vector<std::thread> threads;
    std::vector<double> latencies(num_threads, 0.0);

    g_ready_threads.store(0, std::memory_order_relaxed);
    g_start_flag.store(false, std::memory_order_relaxed);

    // 创建线程
    for (int i = 1; i <= num_threads; ++i) {
        threads.emplace_back(BenchThread, i, is_long_msg, std::ref(latencies[i]));
    }

    // 等待所有线程就绪
    while (g_ready_threads.load(std::memory_order_relaxed) < num_threads) {
        std::this_thread::yield();
    }

    // 记录全局开始时间
    auto global_start = high_resolution_clock::now();

    // 鸣枪开跑！
    g_start_flag.store(true, std::memory_order_release);

    // 等待所有前端线程完成写入
    for (auto& t : threads) {
        t.join();
    }

    // 记录前端完成时间
    auto global_end = high_resolution_clock::now();

    // 停止日志引擎 (这会阻塞直到后端把所有残留日志刷入磁盘)
    KzLog::StopAsyncLogger();

    // 记录后端完全落盘时间
    auto disk_end = high_resolution_clock::now();

    // ============================================================================
    // 数据统计与输出
    // ============================================================================
    double total_frontend_time_sec = duration_cast<duration<double>>(global_end - global_start).count();
    double total_disk_time_sec = duration_cast<duration<double>>(disk_end - global_start).count();
    
    long long total_messages = static_cast<long long>(num_threads) * kMessagesPerThread;
    
    double avg_frontend_latency = 0;
    for (double lat : latencies) avg_frontend_latency += lat;
    avg_frontend_latency /= num_threads;

    double frontend_throughput = total_messages / total_frontend_time_sec / 1'000'000.0; // Mops/sec
    double disk_throughput = total_messages / total_disk_time_sec / 1'000'000.0; // Mops/sec

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "-> Frontend Avg Latency : " << avg_frontend_latency << " ns / msg\n";
    std::cout << "-> Frontend Throughput  : " << frontend_throughput << " Million msgs/sec\n";
    std::cout << "-> Backend+Disk Throughput: " << disk_throughput << " Million msgs/sec\n";
    std::cout << "-> Total Time (to disk) : " << total_disk_time_sec << " seconds\n";
}

int main() {
    // 警告：请确保编译时开启了 -O3 和 -DNDEBUG
    std::cout << "Starting KzLog Benchmark...\n";

    // 建议测试路径：
    // 1. 真实磁盘路径 (测试 I/O 瓶颈): "./benchmark.log"
    // 2. 内存盘路径 (测试纯软件架构极限): "/dev/shm/benchmark.log" (Linux)
    std::string_view log_path = "/dev/shm/benchmark.log"sv; 

    // 初始化日志引擎
    // KzLog::InitAsyncLogger(log_path);

    // const unsigned int num_cores = std::thread::hardware_concurrency();
    // KzThread::ThreadUtils::pin_to_core(0);
    // KzThread::ThreadUtils::enable_local_memory_policy();

    // 场景 1：单线程基准测试 (Baseline)
    RunBenchmark(1, false, log_path);
    RunBenchmark(1, true, log_path);

    // 场景 2：多线程高并发测试 (测试 SPSC 队列和 False Sharing 隔离效果)
    RunBenchmark(4, false, log_path);
    RunBenchmark(8, false, log_path);
    
    // 场景 3：极端并发测试 (如果你的 CPU 有 16 核以上)
    RunBenchmark(16, false, log_path);

    // 场景 4：多线程大日志测试 (测试 Bypass Buffering 旁路机制)
    RunBenchmark(4, true, log_path);

    // 停止日志引擎 (这会阻塞直到后端把所有残留日志刷入磁盘)
    // KzLog::StopAsyncLogger();

    return 0;
}