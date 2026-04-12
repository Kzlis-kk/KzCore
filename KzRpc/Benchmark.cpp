#include "KzRpc/RpcServer.h"
#include "KzRpc/RpcClient.h"
#include "KzNet/EventLoop.h"
#include "KzNet/InetAddress.h"
#include "KzLog/Logger.h"
#include "KzSTL/KzString.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>
#include <future>

using namespace KzRpc;
using namespace KzSTL;

// ============================================================================
// 1. 定义业务数据结构与 ADL 序列化 (The Data & Serialization)
// ============================================================================

struct EchoReq {
    KzString message{};
};

struct EchoResp {
    KzString message{};
};

// ADL 注入：让 Serializer 认识结构体
template <bool UseVarint>
bool operator>>(Deserializer<UseVarint>& d, EchoReq& req) { return d >> req.message; }

template <bool UseVarint>
Serializer<UseVarint>& operator<<(Serializer<UseVarint>& s, const EchoReq& req) { return s << req.message; }

template <bool UseVarint>
bool operator>>(Deserializer<UseVarint>& d, EchoResp& resp) { return d >> resp.message; }

template <bool UseVarint>
Serializer<UseVarint>& operator<<(Serializer<UseVarint>& s, const EchoResp& resp) { return s << resp.message; }


// ============================================================================
// 2. 定义 RPC 服务端逻辑 (The Service)
// ============================================================================

class EchoService {
public:
    // 严格匹配 MethodWrapper 的签名要求
    Expected<void, RpcError> Echo(const EchoReq& req, Serializer<false>& resp_serializer) {
        EchoResp resp;
        resp.message = req.message; // 核心业务：原样返回
        
        // 将响应写入序列化器 (利用设计的 Out-Parameter 模式)
        resp_serializer << resp;
        
        return Expected<void, RpcError>(); 
    }
};


// ============================================================================
// 3. 客户端压测协程 (The Coroutine Runner)
// ============================================================================

std::atomic<int> g_success_count{0};
std::atomic<int> g_fail_count{0};

// 每个协程负责连续发送 N 个请求
Task<void> benchmark_coro(RpcClient* client, int requests_per_coro, const EchoReq& req) {
    for (int i = 0; i < requests_per_coro; ++i) {
        // 发起 RPC 调用并挂起，等待响应
        auto result = co_await client->call<EchoReq, EchoResp, false>(1, 1, req);
        
        if (result.has_value()) {
            g_success_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_fail_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
}


// ============================================================================
// 4. Server 模式启动函数
// ============================================================================
void runServer(uint16_t port, int worker_threads) {
    std::cout << "=========================================" << std::endl;
    std::cout << "      KzRpc Benchmark - SERVER MODE      " << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "Listen Port    : " << port << std::endl;
    std::cout << "Worker Threads : " << worker_threads << std::endl;
    std::cout << "Server is running..." << std::endl;

    KzNet::EventLoop loop;
    KzNet::InetAddress listen_addr(port, true, false);
    
    // 构造并初始化 RpcServer (4个IO线程，N个Worker线程)
    RpcServer server(&loop, listen_addr, 4, worker_threads);
    EchoService echo_service;
    server.registerMethod<false, EchoService, EchoReq, &EchoService::Echo>(1, 1, &echo_service);

    // 阻塞运行事件循环
    loop.loop();
}


// ============================================================================
// 5. Client 模式启动函数
// ============================================================================
void runClient(const std::string& ip, uint16_t port, int poolsize, int num_coroutines, int reqs_per_coro) {
    std::cout << "=========================================" << std::endl;
    std::cout << "      KzRpc Benchmark - CLIENT MODE      " << std::endl;
    std::cout << "=========================================" << std::endl;

    std::promise<RpcClient*> client_started_promise;
    KzNet::EventLoop* g_client_loop = nullptr;
    std::thread client_thread([&]() {
        // 1. 构造 EventLoop (此时绑定到当前后台线程)
        KzThread::ThreadUtils::pin_to_core(3);
        KzThread::ThreadUtils::enable_local_memory_policy();
        KzNet::EventLoop client_loop;
        g_client_loop = &client_loop;
        RpcClient client(&client_loop, KzNet::InetAddress("127.0.0.1", 8080, false), 1); // 启动1个连接

        // 2. 通知主线程：Client 已经就绪，可以开始压测了
        client_started_promise.set_value(&client);

        // 3. 开启事件循环 (阻塞在这里)
        client_loop.loop();
    });

    RpcClient* client = client_started_promise.get_future().get(); // 等待 client 启动就绪
    std::this_thread::sleep_for(std::chrono::seconds(1));  // 等待连接完成

    // 等待连接池建立连接
    std::cout << "Connecting to " << ip << ":" << port << "..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 构造一个 128 字节的 Payload (模拟真实业务包大小)
    EchoReq req{};
    req.message.resize(128, 'K');
    int total_reqs = num_coroutines * reqs_per_coro;

    std::cout << "Payload Size : " << req.message.size() << " bytes" << std::endl;
    std::cout << "Conn Pool    : " << poolsize << " connections" << std::endl;
    std::cout << "Concurrency  : " << num_coroutines << " coroutines" << std::endl;
    std::cout << "Reqs/Coro    : " << reqs_per_coro << std::endl;
    std::cout << "Total Reqs   : " << total_reqs << std::endl;
    std::cout << "Benchmarking..." << std::endl;

    // 点火！(Ignition)
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_coroutines; ++i) {
        auto task = benchmark_coro(client, reqs_per_coro, req);
        task.start_detached();
    }

    // 简单的自旋等待 (每 10ms 检查一次)
    while (g_success_count + g_fail_count < total_reqs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    // 打印报告
    double qps = total_reqs / elapsed.count();
    double avg_latency_us = (elapsed.count() * 1000000.0) / reqs_per_coro;

    std::cout << "\n---------------- Results ----------------" << std::endl;
    std::cout << "Time Elapsed : " << elapsed.count() << " seconds" << std::endl;
    std::cout << "Success      : " << g_success_count << std::endl;
    std::cout << "Failed       : " << g_fail_count << std::endl;
    std::cout << "QPS          : " << static_cast<int>(qps) << " req/sec" << std::endl;
    std::cout << "Avg Latency  : " << avg_latency_us << " us" << std::endl;
    std::cout << "-----------------------------------------" << std::endl;

    // 优雅退出
    g_client_loop->quit();
    client_thread.join();
}


// ============================================================================
// 6. 主函数：解析命令行参数
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  Server: " << argv[0] << " -s [port] [worker_threads]\n"
                  << "  Client: " << argv[0] << " -c [ip] [port][poolsize] [coroutines] [reqs_per_coro]\n\n"
                  << "Examples:\n"
                  << "  " << argv[0] << " -s 8080 8\n"
                  << "  " << argv[0] << " -c 127.0.0.1 8080 8 100 1000\n";
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "-s") {
        uint16_t port = (argc > 2) ? std::stoi(argv[2]) : 8080;
        int worker_threads = (argc > 3) ? std::stoi(argv[3]) : 4;
        runServer(port, worker_threads);
    } 
    else if (mode == "-c") {
        std::string ip = (argc > 2) ? argv[2] : "127.0.0.1";
        uint16_t port = (argc > 3) ? std::stoi(argv[3]) : 8080;
        int poolsize = (argc > 4) ? std::stoi(argv[4]) : 1;
        int coroutines = (argc > 5) ? std::stoi(argv[5]) : 5;
        int reqs_per_coro = (argc > 6) ? std::stoi(argv[6]) : 5;
        runClient(ip, port, poolsize, coroutines, reqs_per_coro);
    } 
    else {
        std::cerr << "Unknown mode: " << mode << "\n";
        return 1;
    }

    return 0;
}