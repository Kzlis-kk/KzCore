#include "KzNet/TcpServer.h"
#include "KzNet/TcpClient.h"
#include "KzNet/EventLoop.h"
#include "KzNet/EventLoopThread.h"
#include "KzNet/InetAddress.h"
#include "KzNet/Buffer.h"
#include "KzLog/Logger.h"
#include "KzThread/ThreadUtils.h"

#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>

using namespace KzNet;
using namespace KzSTL;
using namespace std::chrono_literals;

// ============================================================================
// Server 端逻辑 (Echo Server)
// ============================================================================
class PingPongServer {
public:
    PingPongServer(EventLoop* loop, const InetAddress& listenAddr, size_t threadNum, size_t blockSize) noexcept
        : _server(loop, listenAddr, threadNum, true, "PingPongServer"),
          _blockSize(blockSize)
    {
        _server.setConnectionCallback([](const std::shared_ptr<TcpConnection>& conn) mutable noexcept {
            if (conn->connected()) conn->setTcpNoDelay(true);
        });
        _server.setMessageCallback([this](const std::shared_ptr<TcpConnection>& conn, Buffer* buf, KzTimer::TimeStamp) mutable noexcept {
            while (buf->readableBytes() >= _blockSize) {
                // 极致零拷贝路径：直接将 Buffer 内存映射给内核
                conn->send(std::string_view(buf->peek(), _blockSize));
                buf->retrieve(_blockSize);
            }
        });
    }

    void start() noexcept {
        _server.start(1);
    }

private:
    TcpServer _server;
    size_t _blockSize;
};

void runServer(uint16_t port, int threadCount, size_t blockSize) noexcept {
    KzThread::ThreadUtils::pin_to_core(0);
    KzThread::ThreadUtils::enable_local_memory_policy();
    LOG_INFO << "Starting PingPong Server on port " << port << " with " << threadCount << " threads.";
    EventLoop loop;
    PingPongServer server(&loop, InetAddress(port, false, false), threadCount, blockSize);
    server.start();
    loop.loop();
}

// ============================================================================
// Client 端逻辑 (Load Generator)
// ============================================================================
class PingPongSession : public std::enable_shared_from_this<PingPongSession> {
public:
    PingPongSession(EventLoop* loop, const InetAddress& serverAddr, const KzString& payload, size_t blockSize, int pipeline)
        : _client(std::allocate_shared<TcpClient>(KzAlloc::KzAllocator<TcpClient>(), loop, serverAddr, "PingPongClient")),
          _payload(payload),
          _blockSize(blockSize),
          _pipeline(pipeline)
    {
        _client->setConnectionCallback([this](const std::shared_ptr<TcpConnection>& conn) mutable noexcept {
            if (conn->connected()) {
                conn->setTcpNoDelay(true);
                for (int i = 0; i < _pipeline; ++i) {
                    conn->send(std::string_view(_payload.data(), _payload.size()));
                }
            }
        });
        _client->setMessageCallback([this](const std::shared_ptr<TcpConnection>& conn, Buffer* buf, KzTimer::TimeStamp) mutable noexcept {
            while (buf->readableBytes() >= _blockSize) {
                _messagesRead++;
                
                conn->send(std::string_view(buf->peek(), _blockSize));
                buf->retrieve(_blockSize);
            }
        });
    }

    void start() { _client->connect(); }
    void stop() { _client->disconnect(); }
    uint64_t messagesRead() const { return _messagesRead; }

private:
    std::shared_ptr<TcpClient> _client;
    KzString _payload;
    size_t _blockSize;
    int _pipeline;
    uint64_t _messagesRead = 0;
};


void runClient(const std::string& ip, uint16_t port, int threadCount, int connectionCount, int payloadSize, int pipeline, int timeoutSec) {
    LOG_INFO << "Starting PingPong Client: " << connectionCount << " connections, " 
             << payloadSize << " bytes payload, " << threadCount << " threads.";
    KzThread::ThreadUtils::pin_to_core(7);
    KzThread::ThreadUtils::enable_local_memory_policy();
    EventLoop loop;
    EventLoopThreadPool pool(&loop, threadCount);
    pool.start(5);

    // 构造测试 Payload
    std::string messageStr(payloadSize, 'K');
    KzString payload(messageStr.data(), messageStr.size());

    InetAddress serverAddr(ip, port, false);
    std::vector<std::shared_ptr<PingPongSession>, KzAlloc::KzAllocator<std::shared_ptr<PingPongSession>>> sessions;
    sessions.reserve(connectionCount);

    for (size_t i = 0; i < connectionCount; ++i) {
        EventLoop* ioLoop = pool.getNextLoop();
        auto session = std::allocate_shared<PingPongSession>(KzAlloc::KzAllocator<PingPongSession>(),
                                ioLoop, serverAddr, payload, payloadSize, pipeline);
        session->start();
        sessions.push_back(session);
    }
    // 3. 等待压测结束
    // 设置定时器，在 timeoutSec 后停止 EventLoop
    loop.addTimerInLoop([&loop]() mutable noexcept {
        LOG_INFO << "Timeout reached, stopping benchmark...";
        loop.quit();
    }, KzTimer::TimeStamp::now().addSeconds(timeoutSec));

    auto startTime = std::chrono::high_resolution_clock::now();
    
    // 阻塞运行
    loop.loop();

    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = endTime - startTime;
    // 4. 停止所有连接
    LOG_INFO << "Stopping connections...";
    uint64_t totalMessages = 0;
    for (auto& session : sessions) {
        totalMessages += session->messagesRead();
        session->stop();
    }
    uint64_t totalBytes = totalMessages * payloadSize;
    std::this_thread::sleep_for(1s); // 等待 FIN 包挥手完成
    
    double mbps = static_cast<double>(totalBytes) / 1024 / 1024 / elapsed.count();
    double qps = static_cast<double>(totalMessages) / elapsed.count();

    std::cout << "\n====================================================\n";
    std::cout << " Benchmark Results (" << elapsed.count() << " seconds)\n";
    std::cout << "====================================================\n";
    std::cout << " Payload Size           : " << payloadSize << " bytes\n";
    std::cout << " Connections            : " << connectionCount << "\n";
    std::cout << " Pipeline Depth         : " << pipeline << "\n";
    std::cout << " Throughput (Client Rx) : " << mbps << " MB/s\n";
    std::cout << " Requests Per Second    : " << qps << " QPS\n";
    std::cout << "====================================================\n";
}

// ============================================================================
// Main 入口
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  Server: " << argv[0] << " -s [port] [threads]\n"
                  << "  Client: " << argv[0] << " -c [ip] [port] [threads] [connections] [payload_size] [duration_sec]\n";
        return 1;
    }

    std::string mode = argv[1];
    if (mode == "-s") {
        uint16_t port = (argc > 2) ? std::stoi(argv[2]) : 8080;
        int threads = (argc > 3) ? std::stoi(argv[3]) : 4;
        size_t blockSize = (argc > 4) ? std::stoi(argv[4]) : 16384;
        runServer(port, threads, blockSize);
    } 
    else if (mode == "-c") {
        std::string ip = (argc > 2) ? argv[2] : "127.0.0.1";
        uint16_t port = (argc > 3) ? std::stoi(argv[3]) : 8080;
        int threads = (argc > 4) ? std::stoi(argv[4]) : 2;
        int conns = (argc > 5) ? std::stoi(argv[5]) : 200;
        int payload = (argc > 6) ? std::stoi(argv[6]) : 16384;
        int pipeline = (argc > 7) ? std::stoi(argv[7]) : 10;
        int duration = (argc > 8) ? std::stoi(argv[8]) : 10;
        runClient(ip, port, threads, conns, payload, pipeline, duration);
    }

    return 0;
}