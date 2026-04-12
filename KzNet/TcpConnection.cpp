#include "TcpConnection.h"
#include "EventLoop.h"

#include <unistd.h>
#include <errno.h>

namespace KzNet {

using namespace std::string_view_literals;

TcpConnection::TcpConnection(EventLoop* loop, 
                             int sockfd, 
                             const InetAddress& localAddr, 
                             const InetAddress& peerAddr,
                             std::string_view name) noexcept
    : _loop(loop),
      _name(name),
      _state(StateE::CONNECTING),
      _reading(true),
      _socket(sockfd), // 直接构造内嵌对象
      _channel(loop, sockfd), // 直接构造内嵌对象
      _localAddr(localAddr),
      _peerAddr(peerAddr),
      _highWaterMark(64 * 1024 * 1024) // 默认 64MB
{
    _channel.setReadCallback([this](KzTimer::TimeStamp t) mutable noexcept { this->handleRead(t); });
    _channel.setWriteCallback([this] mutable noexcept { this->handleWrite(); });
    _channel.setCloseCallback([this] mutable noexcept { this->handleClose(); });
    _channel.setErrorCallback([this] mutable noexcept { this->handleError(); });
}


void TcpConnection::connectEstablished() noexcept {
    // 必须在 Loop 线程
    assert(_loop->isInLoopThread());
    // 如果在此期间已经被 forceClose 标记为 DISCONNECTING，则直接返回
    if (_state.load(std::memory_order_acquire) != StateE::CONNECTING) {
        return; 
    }

    setState(StateE::CONNECTED);

    _channel.enableReading();

    assert(_connectionCallback);
    LOG_CONN(shared_from_this(), "connectEstablished: before user callback");
    _connectionCallback(shared_from_this());
    LOG_CONN(shared_from_this(), "connectEstablished: after user callback");
}

void TcpConnection::connectDestroyed() noexcept {
    assert(_loop->isInLoopThread());

    StateE s = _state.load(std::memory_order_acquire);
    if (s == StateE::CONNECTED || s == StateE::DISCONNECTING || s == StateE::CONNECTING) {
        setState(StateE::DISCONNECTED);
        _channel.disableAll();
        assert(_connectionCallback);
        _connectionCallback(shared_from_this());
    }
    _channel.remove();
    // 此时 TcpConnection 的引用计数应该降为 0 (如果用户没持有)，析构函数将被调用
    // 如果用户持有，那么此时 TcpConnection 已经断开连接，使用 TcpConnection 前必须先检查 _state 
}

void TcpConnection::handleRead(KzTimer::TimeStamp receiveTime) noexcept {
    assert(_loop->isInLoopThread());
    
    // 使用 Hybrid LT 策略读取 (Buffer 内部循环 16 次)
    ssize_t n = _inputBuffer.readFd(_channel.fd());

    if (n > 0) {
        // 读到数据，回调用户
        assert(_messageCallback);
        _messageCallback(shared_from_this(), &_inputBuffer, receiveTime);
    } 
    else if (n == 0) {
        // 对端关闭 (EOF)
        handleClose();
    } 
    else {
        // 错误 (errno 已经在 readFd 内部检查过了，如果是 EAGAIN 会返回 >0 或 -1)
        // readFd 返回 -1 且 errno != EAGAIN 才是真错误
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_SYSERR << "TcpConnection::handleRead";
            handleError();
            forceCloseInLoop();
        }
    }
}

void TcpConnection::handleClose() noexcept {
    assert(_loop->isInLoopThread());
    
    assert(_state.load(std::memory_order_acquire) == StateE::CONNECTED || _state.load(std::memory_order_acquire) == StateE::DISCONNECTING);
    setState(StateE::DISCONNECTED);
    
    _channel.disableAll();

    std::shared_ptr<TcpConnection> guardThis(shared_from_this());
    LOG_CONN(guardThis, "handleClose: created guardThis");

    // 回调用户 (连接断开)
    assert(_connectionCallback);
    _connectionCallback(guardThis);
    LOG_CONN(guardThis, "handleClose: created guardThis");

    // 通知 Server 移除连接 (延迟销毁的触发点)
    // closeCallback 绑定的是 TcpServer::removeConnection
    // Server 会在 removeConnection 中调用 queueInLoop(connectDestroyed)
    assert(_closeCallback);
    _closeCallback(guardThis);
    LOG_CONN(guardThis, "handleClose: created guardThis");

    // 清空协程等待队列
    // 唤醒所有等待的协程，让它们进入 await_resume -> 发现连接断开 -> 退出
    while (!_blockedCoroutines.empty()) {
        auto h = _blockedCoroutines.pop();
        if (h) h.resume();
    }
}

void TcpConnection::sendInLoop(KzSTL::KzString&& message) noexcept {
    ssize_t nwrote = 0;
    size_t remaining = message.size();

    if (_state.load(std::memory_order_acquire) == StateE::DISCONNECTED) [[unlikely]] {
        LOG_WARN << "disconnected, give up writing";
        return;
    }

    // Direct Write 尝试
    // 只有当 Buffer 和 Queue 都为空时，才能直接写，保证顺序
    if (!_channel.isWriting() && _outputQueue.empty()) {
        nwrote = ::send(_channel.fd(), message.data(), message.size(), MSG_NOSIGNAL);
        if (nwrote >= 0) {
            remaining = message.size() - nwrote;
            if (remaining == 0 && _writeCompleteCallback) {
                _loop->queueInLoop([conn = shared_from_this()] mutable noexcept {
                    if (conn->_writeCompleteCallback) conn->_writeCompleteCallback(conn);
                });
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                LOG_SYSERR << "TcpConnection::sendInLoop";
                if (errno == EPIPE || errno == ECONNRESET) {
                    // 对端关闭或连接重置
                    forceCloseInLoop(); // 强制关闭
                    return;
                }
            }
        }
    }

    // 积压处理
    if (remaining > 0) {
        size_t oldLen = _outputQueue.totalBytes();
        
        // 高水位检查
        if (oldLen + remaining >= _highWaterMark && 
            oldLen < _highWaterMark) 
        {
            _loop->queueInLoop([conn = shared_from_this(), len = oldLen + remaining] mutable noexcept {
                callbacks::defaultHighWaterMarkCallback(conn, len);
            });
        }

        // 统一压入队列 (内部自动处理小包聚合与大包零拷贝)
        _outputQueue.push_or_append(std::move(message), static_cast<size_t>(nwrote));
        
        if (!_channel.isWriting()) {
            _channel.enableWriting();
        }
    }
}

void TcpConnection::sendInLoop(std::string_view message) noexcept {
    ssize_t nwrote = 0;
    size_t remaining = message.size();

    if (_state.load(std::memory_order_acquire) == StateE::DISCONNECTED) [[unlikely]] {
        LOG_WARN << "disconnected, give up writing";
        return;
    }

    // Direct Write 尝试 (零拷贝路径)
    if (!_channel.isWriting() && _outputQueue.empty()) {
        nwrote = ::send(_channel.fd(), message.data(), message.size(), MSG_NOSIGNAL);
        if (nwrote >= 0) {
            remaining = message.size() - nwrote;
            if (remaining == 0 && _writeCompleteCallback) {
                _loop->queueInLoop([conn = shared_from_this()] mutable noexcept {
                    if (conn->_writeCompleteCallback) conn->_writeCompleteCallback(conn);
                });
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                LOG_SYSERR << "TcpConnection::sendInLoop";
                if (errno == EPIPE || errno == ECONNRESET) {
                    // 对端关闭或连接重置
                    forceCloseInLoop(); // 强制关闭
                    return;
                }
            }
        }
    }

    // 积压处理
    if (remaining > 0) {
        size_t oldLen = _outputQueue.totalBytes();
        
        // 高水位检查
        if (oldLen + remaining >= _highWaterMark && 
            oldLen < _highWaterMark) 
        {
            _loop->queueInLoop([conn = shared_from_this(), len = oldLen + remaining] mutable noexcept {
                callbacks::defaultHighWaterMarkCallback(conn, len);
            });
        }

        // 统一压入队列 (内部自动处理小包聚合与大包零拷贝)
        _outputQueue.push_or_append(std::string_view(message.data() + nwrote, remaining));
        
        if (!_channel.isWriting()) {
            _channel.enableWriting();
        }
    }
}

void TcpConnection::handleWrite() noexcept {
    // 准备 iovec 数组 (栈上分配)
    constexpr int kMaxIov = 16;
    struct iovec iov[kMaxIov];
    int iovcnt = _outputQueue.fillIov(iov, kMaxIov);

    // 批量发送 (Scatter/Gather IO)
    ssize_t n = ::writev(_channel.fd(), iov, iovcnt);

    if (n > 0) [[likely]] {
        // 消费队列
        _outputQueue.consume(static_cast<size_t>(n));

        // 检查是否全部发完
        if (_outputQueue.empty()) {
            _channel.disableWriting();
                
            // 低水位 (Write Complete)
            if (_writeCompleteCallback) {
                _loop->queueInLoop([conn = shared_from_this()] mutable noexcept {
                    conn->_writeCompleteCallback(conn);
                });
            }
            if (_state.load(std::memory_order_acquire) == StateE::DISCONNECTING) {
                shutdownInLoop();
            }
        }
            
        // 协程唤醒
        // 只要有数据发出去，水位下降了，就可以尝试唤醒被阻塞的协程
        // 这里只要 totalBytes < HWM/2，就唤醒等待者
        if (_outputQueue.totalBytes() < _highWaterMark / 2) {
            checkCoroutineResume();
        }
    } 
    else {
        if (errno != EWOULDBLOCK && errno != EAGAIN) [[unlikely]] {
            LOG_SYSERR << "TcpConnection::handleWrite error";
            // 真正的错误应该触发关闭
            if (errno == EPIPE || errno == ECONNRESET) forceCloseInLoop();
        }
    }
}

void TcpConnection::checkCoroutineResume() noexcept {
    // 在唤醒协程期间，持有 shared_ptr，防止协程内部操作导致连接被意外析构
    std::shared_ptr<TcpConnection> guard(shared_from_this());

    // 唤醒所有因背压而挂起的协程
    // 全部唤醒可能导致瞬间又把 Buffer 填满。
    // 但由于协程是串行恢复的，第一个恢复的协程填满 Buffer 后，
    // 后续协程在 await_ready 检查时会再次挂起。

    // 限制单次唤醒数量，防止 IO 线程被协程恢复逻辑卡死 (惊群效应防御)
    int resumeCount = 0;
    constexpr int kMaxResumePerLoop = 16;
    
    while (!_blockedCoroutines.empty() && resumeCount < kMaxResumePerLoop) {
        auto h = _blockedCoroutines.pop(); // O(1)
        if (h) {
            h.resume();
            resumeCount++;
        }
        
        // 如果唤醒一个后，Buffer 又满了，停止唤醒
        if (_outputQueue.totalBytes() >= _highWaterMark) {
            break;
        }
    }
}

// === 关闭逻辑 ===

void TcpConnection::shutdown() noexcept {
    StateE expected = StateE::CONNECTED;
    if (_state.compare_exchange_strong(expected, StateE::DISCONNECTING, std::memory_order_acq_rel)) {
        _loop->runInLoop([conn = shared_from_this()] mutable noexcept { conn->shutdownInLoop(); });
    }
}

void TcpConnection::shutdownInLoop() noexcept {
    assert(_loop->isInLoopThread());
    // 只有当 OutputBuffer 写完时，才真正关闭写端
    if (!_channel.isWriting()) {
        _socket.shutdownWrite();
    }
}

void TcpConnection::forceClose() noexcept {
    StateE state = _state.load(std::memory_order_acquire);
    if (state == StateE::CONNECTED || state == StateE::DISCONNECTING || state == StateE::CONNECTING) {
        setState(StateE::DISCONNECTING);
        _loop->queueInLoop([conn = shared_from_this()] mutable noexcept { conn->forceCloseInLoop(); });
    }
}

void TcpConnection::forceCloseInLoop() noexcept {
    assert(_loop->isInLoopThread());
    StateE state = _state.load(std::memory_order_acquire);
    if (state == StateE::CONNECTED || state == StateE::DISCONNECTING || state == StateE::CONNECTING) {
        handleClose();
    }
}

std::string_view TcpConnection::stateToString() const noexcept {
    switch (_state) {
        case StateE::DISCONNECTED: return "DISCONNECTED"sv;
        case StateE::CONNECTING: return "CONNECTING"sv;
        case StateE::CONNECTED: return "CONNECTED"sv;
        case StateE::DISCONNECTING: return "DISCONNECTING"sv;
        default: return "unknown state"sv;
    }
}

} // namespace KzNet