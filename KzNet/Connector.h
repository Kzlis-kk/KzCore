#pragma once

#include <memory>
#include <atomic>
#include <cerrno>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include "Channel.h"
#include "Socket.h"
#include "InetAddress.h"
#include "EventLoop.h"
#include "KzTimer/TimerId.h"
#include "KzSTL/Function.h"
#include "KzLog/Logger.h"
#include "KzTimer/TimeStamp.h"

namespace KzNet {

/**
 * @brief TCP 连接发起器 (Non-blocking Connect)
 * 
 * * 核心职责：
 *   1. 封装非阻塞 connect 的复杂状态机 (EINPROGRESS 处理)。
 *   2. 处理连接成功、失败、以及自连接 (Self-Connect) 等边缘情况。
 *   3. 实现带有指数退避 (Exponential Backoff) 的自动重连逻辑。
 * 
 * * 线程模型：
 *   继承 enable_shared_from_this，生命周期由 TcpClient (shared_ptr) 管理。
 *   所有内部状态修改 (startInLoop, handleWrite 等) 严格限制在其所属的 EventLoop 线程中。
 */
class Connector : public std::enable_shared_from_this<Connector> {
public:
    // 连接成功回调：传递新连接的 fd 给 TcpClient
    using NewConnectionCallback = KzSTL::Function<void(int)>;

    Connector(EventLoop* loop, const InetAddress& serverAddr) noexcept
    : _loop(loop),
      _serverAddr(serverAddr),
      _connect(false),
      _channel(loop, -1),
      _state(StateE::DISCONNECTED),
      _retryDelayMs(kInitRetryDelayMs)
{}
    ~Connector() {
        // 析构时必须取消定时器，防止Use-After-Free
        _loop->cancel(_retryTimerId);
    }

    Connector(const Connector&) = delete;
    Connector& operator=(const Connector&) = delete;

    void setNewConnectionCallback(const NewConnectionCallback& cb) { _newConnectionCallback = cb; }
    void setNewConnectionCallback(NewConnectionCallback&& cb) { _newConnectionCallback = std::move(cb); }

    // 开始连接 (线程安全，可跨线程调用)
    void start() noexcept {
        _connect.store(true);
        // 线程安全：将启动逻辑投递到 Loop 线程
        // 捕获 shared_ptr，确保 Lambda 执行时 Connector 活着
        _loop->runInLoop([self = shared_from_this()] mutable noexcept { self->startInLoop(); });
    }
    
    // 重新开始连接 (用于断线重连，必须在 Loop 线程调用)
    void restart() noexcept {
        assert(_loop->isInLoopThread());
        setState(StateE::DISCONNECTED);
        _retryDelayMs = kInitRetryDelayMs;
        _connect.store(true);
        startInLoop();
    }
    
    // 停止连接过程 (线程安全，可跨线程调用)
    void stop() noexcept {
        _connect.store(false);
        // 线程安全：取消定时器必须在 Loop 线程
        _loop->runInLoop([self = shared_from_this()] mutable noexcept { self->stopInLoop(); });
    }

    const InetAddress& serverAddress() const noexcept { return _serverAddr; }

private:
    // 连接状态机
    enum class StateE { DISCONNECTED, CONNECTING, CONNECTED };

    // 仅在 Loop 线程调用
    void setState(StateE s) noexcept { _state = s; }

    void startInLoop() noexcept {
        assert(_loop->isInLoopThread());
        assert(_state == StateE::DISCONNECTED);
        if (_connect) [[likely]] {
            connect();
        }
    }
    void stopInLoop() noexcept {
        assert(_loop->isInLoopThread());
        if (_state == StateE::CONNECTING) {
            int sockfd = removeAndResetChannel();
            cleanup(sockfd);
        }
    }

    void connect() noexcept;
    void connecting(int sockfd) noexcept;
    void cleanup(int sockfd) noexcept {
        if (sockfd >= 0) {
            ::close(sockfd);
        }
        setState(StateE::DISCONNECTED);
    }

    // Channel 回调
    void handleWrite() noexcept;
    void handleError() noexcept {
        LOG_ERROR << "Connector::handleError state=" << static_cast<int>(_state);
        if (_state == StateE::CONNECTING) {
            int sockfd = removeAndResetChannel();
            int err = Socket::getSocketError(sockfd);
            retry(sockfd);
        }
    }

    // 重连逻辑
    void retry(int sockfd) noexcept;
    
    // Channel 生命周期管理
    int removeAndResetChannel() noexcept {
        _channel.disableAll();
        _channel.remove();
        int sockfd = _channel.fd();
    
        // 即使当前 channel 在 handleEvent 也不要紧，因为不会访问 fd
        _channel.changeFd(-1);
    
        return sockfd;
    }
    void resetChannel() noexcept { _channel.changeFd(-1); }

private:
    EventLoop* _loop;
    InetAddress _serverAddr;
    std::atomic<bool> _connect; // 标志位：控制是否允许发起连接/重连
    StateE _state;              // 当前连接状态
    int _retryDelayMs;          // 重连毫秒
    Channel _channel;           // 监听 connect 结果的 Channel
    NewConnectionCallback _newConnectionCallback;
    KzTimer::TimerId _retryTimerId; // 重连定时器 ID，用于析构时取消

    // 指数退避重连参数
    static constexpr int kMaxRetryDelayMs = 30 * 1000; // 最大重连延迟 30s
    static constexpr int kInitRetryDelayMs = 500;      // 初始重连延迟 0.5s
};

inline void Connector::connect() noexcept {
    int sockfd = Socket::createNonblockingOrDie(_serverAddr.family());
    int ret = ::connect(sockfd, _serverAddr.getSockAddr(), _serverAddr.static_addr_len());
    
    int savedErrno = (ret == 0) ? 0 : errno;
    switch (savedErrno) {
        case 0:
        case EINPROGRESS: // 非阻塞 connect 的正常返回值
        case EINTR:       // 被信号中断
        case EISCONN:     // 连接已建立
            connecting(sockfd);
            break;

        case EAGAIN:
        case EADDRINUSE:
        case EADDRNOTAVAIL:
        case ECONNREFUSED:
        case ENETUNREACH:
            retry(sockfd); // 可恢复的错误，进行重连
            break;

        case EACCES:
        case EPERM:
        case EAFNOSUPPORT:
        case EALREADY:
        case EBADF:
        case EFAULT:
        case ENOTSOCK:
            // 致命错误，关闭 socket 并 log
            LOG_SYSERR << "connect error in Connector::startInLoop " << savedErrno;
            ::close(sockfd);
            break;

        default:
            LOG_SYSERR << "Unexpected error in Connector::startInLoop " << savedErrno;
            ::close(sockfd);
            break;
    }
}

inline void Connector::connecting(int sockfd) noexcept {
    setState(StateE::CONNECTING);
    
    if (_channel.fd() != -1) {
        _channel.disableAll();
        _channel.remove();
    }
    _channel.changeFd(sockfd);
    
    _channel.setWriteCallback([this] mutable noexcept { this->handleWrite(); });
    _channel.setErrorCallback([this] mutable noexcept { this->handleError(); });

    // 关注可写事件：非阻塞 connect 成功或失败时，socket 都会变为可写
    _channel.enableWriting();
}

inline void Connector::handleWrite() noexcept {
    if (_state == StateE::CONNECTING) {
        int sockfd = removeAndResetChannel();
        int err = Socket::getSocketError(sockfd);
        
        if (err) [[unlikely]] {
            // 连接失败
            LOG_WARN << "Connector::handleWrite - SO_ERROR = " << err << " " << strerror(err);
            retry(sockfd);
        } else if (Socket::isSelfConnect(sockfd)) [[unlikely]] { 
            // 检查自连接 (Self-Connect)：源 IP:Port 和 目的 IP:Port 完全一样
            // 这种情况在本地测试且端口耗尽时极小概率发生，必须断开重连
            LOG_WARN << "Connector::handleWrite - Self connect";
            retry(sockfd);
        } else {
            // 连接成功
            setState(StateE::CONNECTED);
            if (_connect) {
                _newConnectionCallback(sockfd);
            } else {
                // 如果在连接成功的瞬间，用户调用了 stop()，则直接关闭
                ::close(sockfd);
            }
        }
    }
}

inline void Connector::retry(int sockfd) noexcept {
    cleanup(sockfd); // 先清理旧资源
    
    if (_connect.load()) {
        
        // 安全性保证：Connector 是 TcpClient 的成员，
        // 如果 TcpClient 析构，Connector 会先析构，析构函数会 cancel 定时器。
        // 捕获 weak_ptr，如果 Connector 被析构，定时器触发时什么都不做
        _retryTimerId = _loop->addTimerInLoop([weak_self = weak_from_this()] mutable noexcept {
            if (auto self = weak_self.lock()) {
                self->startInLoop();
            } else {
                LOG_WARN << "Connector destroyed, abort retry.";
            }
        },
        KzTimer::TimeStamp::now().addNano(static_cast<int64_t>(_retryDelayMs) * 1000 * 1000), 
        0, 
        false
        );
        
        // 指数退避
        _retryDelayMs = std::min(_retryDelayMs * 2, kMaxRetryDelayMs);
    }
}

} // namespace KzNet