#pragma once

#include <mutex>
#include <string>
#include <memory>
#include <string_view>
#include <unistd.h>

#include "KzSTL/SpinMutex.h"
#include "TcpConnection.h"
#include "Connector.h"
#include "KzSTL/Function.h"
#include "KzSTL/KzString.h"
#include "KzSTL/Task.h"


namespace KzNet {

class EventLoop;

/**
 * @brief TCP 客户端 (策略层)
 * 
 * * 核心职责：
 *   1. 管理一个 TcpConnection 的生命周期。
 *   2. 提供 connect/disconnect 接口。
 *   3. 实现自动重连策略。
 * 
 */
class TcpClient {
public:
    // 回调定义W
    using ConnectionCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&,
        Buffer*, KzTimer::TimeStamp)>;
    using WriteCompleteCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&)>;

    TcpClient(EventLoop* loop,
              const InetAddress& serverAddr,
              std::string_view name) noexcept;
    ~TcpClient() noexcept;

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // === 核心控制 ===
    void connect() noexcept;
    void disconnect() noexcept;
    void stop() noexcept; // 停止重连
    std::shared_ptr<TcpConnection> connection() const;

    // === 状态查询 ===
    EventLoop* getLoop() const noexcept { return _loop; }
    bool retry() const noexcept { return _retry; }
    void enableRetry() noexcept { _retry = true; }
    std::string_view name() const noexcept { return std::string_view(_name); }

    // === 用户回调设置 ===
    void setConnectionCallback(ConnectionCallback&& cb) noexcept { _connectionCallback = std::move(cb); }
    void setConnectionCallback(const ConnectionCallback& cb) noexcept { _connectionCallback = cb; }
    void setMessageCallback(MessageCallback&& cb) noexcept { _messageCallback = std::move(cb); }
    void setMessageCallback(const MessageCallback& cb) noexcept { _messageCallback = cb; }
    void setWriteCompleteCallback(WriteCompleteCallback&& cb) noexcept { _writeCompleteCallback = std::move(cb); }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) noexcept { _writeCompleteCallback = cb; }

private:
    // 内部回调 (由 Connector 调用)
    void newConnection(int sockfd) noexcept;

    // 内部回调 (由 TcpConnection 调用)
    void removeConnection(const std::shared_ptr<TcpConnection>& conn) noexcept;

    EventLoop* _loop;
    std::shared_ptr<Connector> _connector;  // 负责发起连接
    KzSTL::KzString _name;

    // 状态
    std::atomic<bool> _retry;   // 是否启用重连
    std::atomic<bool> _connect; // 是否发起连接
    int _nextConnId;

    // 保护 _connection 的生命周期和访问
    mutable KzSTL::SpinMutex _mutex;
    std::shared_ptr<TcpConnection> _connection;

    // 用户回调
    ConnectionCallback _connectionCallback;
    MessageCallback _messageCallback;
    WriteCompleteCallback _writeCompleteCallback;
};

inline std::shared_ptr<TcpConnection> TcpClient::connection() const {
    std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
    return _connection;
}

inline void TcpClient::connect() noexcept {
    _connect.store(true, std::memory_order_release);

    // TcpClient 生命周期必须和 Connector 同步
    _connector->setNewConnectionCallback([this](int sockfd) mutable noexcept {
            this->newConnection(sockfd);
        });

    _connector->start();
}

inline void TcpClient::disconnect() noexcept {
    _connect.store(false, std::memory_order_release);

    std::shared_ptr<TcpConnection> conn;
    {
        std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
        conn = _connection;
    }
    if (conn) {
        conn->shutdown();
    }
}

inline void TcpClient::stop() noexcept {
    _connect.store(false, std::memory_order_release);
    _connector->stop();
}

} // namespace KzNet