#pragma once

#include <map>
#include <string>
#include <memory>
#include <atomic>
#include <string_view>
#include <vector>
#include <cstdio>
#include <cassert>
#include <cstring>

#include "KzSTL/KzString.h"
#include "TcpConnection.h"
#include "KzSTL/Function.h"
#include "Acceptor.h"
#include "EventLoopThreadPool.h"
#include "EventLoop.h"
#include "KzLog/Logger.h"
#include "KzAlloc/KzAllocator.h"
#include "KzSTL/SpinMutex.h"
#include "ConnTracer.h"

using namespace std::string_view_literals;

namespace KzNet {
/**
 * @brief TCP 服务器 (Master-Slave Reactor)
 * 
 * * 职责：
 *   1. 管理 Acceptor 和 EventLoopThreadPool。
 *   2. 创建、管理、销毁 TcpConnection。
 *   3. 向用户暴露事件回调接口。
 * 
 */
class TcpServer {
public:
    using ThreadInitCallback = KzSTL::Function<void(EventLoop*)>;
    using ConnectionCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&,
        Buffer*, KzTimer::TimeStamp)>;
    using WriteCompleteCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&)>;

    TcpServer(EventLoop* loop,
              const InetAddress& listenAddr,
              size_t threadNum = 8,
              bool reusePort = false,
              std::string_view name = "TcpServer"sv) noexcept
        : _loop(loop),
          _listenAddr(listenAddr),
          _acceptor(loop, listenAddr, reusePort),
          _threadPool(loop, threadNum),
          _name(name),
          _connectionCallback(callbacks::defaultConnectionCallback),
          _messageCallback(callbacks::defaultMessageCallback)          
    {
        // 将 Acceptor 的新连接回调绑定到 TcpServer::newConnection
        // 直接捕获 this，假设 Server 生命周期长于回调
        _acceptor.setNewConnectionCallback(
            [this](int sockfd, const InetAddress& peerAddr) mutable noexcept { 
                this->newConnection(sockfd, peerAddr);
            });
        
        // 预留一定的 fd 空间，避免频繁扩容
        _connections.resize(64);
        _connections2.resize(64);
    }

    ~TcpServer() {
        assert(_loop->isInLoopThread());

        _destroying.store(true, std::memory_order_release); // 标记正在销毁
        // 关闭所有连接
        for (auto& conn : _connections) {
            if (conn) {
                std::shared_ptr<TcpConnection> connCopy(conn);
                conn.reset(); // 释放 Server 对连接的引用

                // 斩断回调，防止 IO 线程回调已经死亡的 Server
                connCopy->setCloseCallback([](const std::shared_ptr<TcpConnection>&) mutable noexcept {});
                
                // 销毁连接：必须在连接所属的 Loop 中执行
                connCopy->getLoop()->runInLoop(
                    [connCopy]() mutable noexcept { connCopy->connectDestroyed(); });
            }
        }
    }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    // === 核心控制 ===

    // 启动服务器 (启动 IO 线程池，开始监听)
    void start(int first_cpu = 0) noexcept {
        assert(_loop->isInLoopThread());
        bool expected = false;
        if (_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // 启动 IO 线程池
            // _threadPool 是对象，用 . 访问
            _threadPool.start(first_cpu, _threadInitCallback, _name);
            
            // 确保 Acceptor 在 Loop 线程中开始监听
            // Acceptor 内部有断言，这里直接 runInLoop 安全
            _loop->runInLoop([this]() mutable noexcept { 
                // 再次检查，防止重复 listen
                if (!this->_acceptor.listening()) {
                    this->_acceptor.listen(); 
                }
            });
        }
    }
    
    // === 用户回调设置 ===
    // 线程安全
    void setThreadInitCallback(ThreadInitCallback&& cb) noexcept { _threadInitCallback = std::move(cb); }
    void setThreadInitCallback(const ThreadInitCallback& cb) noexcept { _threadInitCallback = cb; }
    void setConnectionCallback(ConnectionCallback&& cb) noexcept { _connectionCallback = std::move(cb); }
    void setConnectionCallback(const ConnectionCallback& cb) noexcept { _connectionCallback = cb; }
    void setMessageCallback(MessageCallback&& cb) noexcept { _messageCallback = std::move(cb); }
    void setMessageCallback(const MessageCallback& cb) noexcept { _messageCallback = cb; }
    void setWriteCompleteCallback(WriteCompleteCallback&& cb) noexcept { _writeCompleteCallback = std::move(cb); }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) noexcept { _writeCompleteCallback = cb; }

    KzSTL::KzString ipPort() const noexcept { 
        // 临时格式化，虽然有开销，但调用频率极低
        char buf[64];
        size_t len = _listenAddr.toIpPort(buf, sizeof(buf));
        return KzSTL::KzString(buf, len);
    }
    EventLoop* getLoop() const noexcept { return _loop; }

private:
    // 内部回调 (由 Acceptor 调用)
    void newConnection(int sockfd, const InetAddress& peerAddr) noexcept {
        assert(_loop->isInLoopThread());
        
        // 负载均衡：选择一个 IO Loop
        EventLoop* ioLoop = _threadPool.getNextLoop();
        
        // 生成连接名
        char buf[256];
        // 内部保证不会使用超过64字节
        size_t n = _listenAddr.toIpPort(buf, sizeof(buf));

        buf[n++] = '-';
        size_t nameLen = std::min(_name.size(), static_cast<size_t>(236 - n));
        ::memcpy(buf + n, _name.data(), nameLen);
        n += nameLen;

        buf[n++] = '#';
        char * end = KzAlgorithm::Jeaiii::to_chars(buf + n, _nextConnId);
        ++_nextConnId;

        // 创建 TcpConnection
        auto conn = std::allocate_shared<TcpConnection>(
            KzAlloc::KzAllocator<TcpConnection>(),
            ioLoop, 
            sockfd, 
            _listenAddr, 
            peerAddr,
            std::string_view(buf, end - buf)
        );

        // 存入 Vector
        if (sockfd >= static_cast<int>(_connections.size())) {
            _connections.resize(sockfd + 16); // 稍微多给点，避免频繁 resize
        }
        {       
            std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
            _connections[sockfd] = conn;
        }

        // 设置回调
        assert(_connectionCallback);
        conn->setConnectionCallback(_connectionCallback);
        assert(_messageCallback);
        conn->setMessageCallback(_messageCallback);
        if (_writeCompleteCallback) conn->setWriteCompleteCallback(_writeCompleteCallback);
        
        // 设置关闭回调：直接捕获 this
        conn->setCloseCallback(
            [this](const std::shared_ptr<TcpConnection>& c) mutable noexcept { 
                this->removeConnection(c); });

        // 启动连接 (在 IO 线程)
        ioLoop->runInLoop([conn]() mutable noexcept { 
            conn->connectEstablished(); });
    }
    
    // 内部回调 (由 TcpConnection 调用)
    void removeConnection(const std::shared_ptr<TcpConnection>& conn) noexcept {
        // 如果 Server 正在销毁，直接放弃清理，因为 ~TcpServer 已经接管了所有连接的后事
        if (_destroying.load(std::memory_order_acquire)) return; 
        // 必须回到主 Loop 来操作 _connections
        _loop->runInLoop([this, conn]() mutable noexcept { this->removeConnectionInLoop(conn); });
    }
    void removeConnectionInLoop(const std::shared_ptr<TcpConnection>& conn) noexcept {
        assert(_loop->isInLoopThread());

        int fd = conn->fd();
        if (fd >= 0 && fd < static_cast<int>(_connections.size())) {
            auto& entry = _connections[fd];
            // 必须校验 entry 是否等于 conn，防止 FD 被复用导致误删新连接
            if (entry == conn) {
                assert(conn.use_count() > 1);
                entry.reset(); // 从 Server 中移除
            }
        }

        // 延迟销毁：在连接所属的 IO 线程中执行 connectDestroyed
        conn->getLoop()->queueInLoop([conn]() mutable noexcept { 
            conn->connectDestroyed(); });
    }

    using ConnectionMap = std::vector<std::shared_ptr<TcpConnection>>;

     // 状态
    std::atomic<bool> _started{false};
    std::atomic<bool> _destroying{false};
    int _nextConnId = 1; // 用于生成连接名

    EventLoop* _loop; // 主 Reactor
    InetAddress _listenAddr;
    KzSTL::KzString _name;

    Acceptor _acceptor;
    ConnectionMap _connections; // 管理所有连接
    EventLoopThreadPool _threadPool;

    // 回调
    ConnectionCallback _connectionCallback;
    MessageCallback _messageCallback;
    WriteCompleteCallback _writeCompleteCallback;
    ThreadInitCallback _threadInitCallback;
    
};

} // namespace KzNet