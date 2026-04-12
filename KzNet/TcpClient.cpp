#include "TcpClient.h"
#include "EventLoop.h"
#include "Socket.h"
#include "KzLog/Logger.h"
#include "KzAlgorithm/Jeaiii.h"
#include "KzAlloc/KzAllocator.h"

#include <cstdio>

namespace KzNet {

TcpClient::TcpClient(EventLoop* loop,
                     const InetAddress& serverAddr,
                     std::string_view nameArg) noexcept
    : _loop(loop),
      _connector(std::allocate_shared<Connector>(
            KzAlloc::KzAllocator<Connector>(), loop, serverAddr)),
      _name(nameArg),
      _retry(false),
      _connect(true),
      _nextConnId(1),
      _connectionCallback(callbacks::defaultConnectionCallback),
      _messageCallback(callbacks::defaultMessageCallback)
{}

TcpClient::~TcpClient() noexcept {
    std::shared_ptr<TcpConnection> conn;
    {
        std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
        conn = _connection; // 增加引用计数
    }

    if (conn) {
        assert(_loop == conn->getLoop());
        // 强制关闭有延迟，必须先把 conn 所有使用到 TcpClient 的回调更换，另两个用户注册回调强制要求不能使用 TcpClient，
        // 因为 TcpClient 仅仅只是管理类，而那两个回调是业务回调
        conn->setCloseCallback([](const std::shared_ptr<TcpConnection>& c) mutable noexcept {
            c->getLoop()->queueInLoop([c] mutable noexcept { c->connectDestroyed(); });
        });
        // 强制关闭
        conn->forceClose();
    } else {
        // 只有在没有建立连接（还在 connecting 阶段）时，才需要停止 Connector
        _connector->stop();
    }
}

void TcpClient::removeConnection(const std::shared_ptr<TcpConnection>& conn) noexcept {
    assert(_loop->isInLoopThread());
    assert(_loop == conn->getLoop());

    {
        std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
        if (_connection == conn) { _connection.reset(); }
    }

    // 延迟销毁
    _loop->queueInLoop([conn] mutable noexcept {
        conn->connectDestroyed();
    });

    // 如果启用了重连，且用户没有调用 stop()，则开始重连
    if (_retry && _connect.load()) {
        _connector->restart();
    }
}

void TcpClient::newConnection(int sockfd) noexcept {
    assert(_loop->isInLoopThread());
    
    InetAddress peerAddr(Socket::getPeerAddr(sockfd));
    char buf[256];
    size_t n = peerAddr.toIpPort(buf, 64);
    buf[n++] = '-';
    size_t nameLen = std::min(_name.size(), static_cast<size_t>(236 - n));
    ::memcpy(buf + n, _name.data(), nameLen);
    n += nameLen;
    buf[n++] = '#';
    char* end = KzAlgorithm::Jeaiii::to_chars(buf + n, _nextConnId);

    ++_nextConnId;

    InetAddress localAddr(Socket::getLocalAddr(sockfd));
    
    auto conn = std::allocate_shared<TcpConnection>(
            KzAlloc::KzAllocator<TcpConnection>(), // Allocator
            _loop, 
            sockfd, 
            localAddr, 
            peerAddr,
            std::string_view(buf, end - buf)
        );

    // 加锁读取用户设置的回调
    {
        std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
        conn->setConnectionCallback(_connectionCallback);
        conn->setMessageCallback(_messageCallback);
        if (_writeCompleteCallback) conn->setWriteCompleteCallback(_writeCompleteCallback);
    }
    
    // 此时 TcpClient 的生命周期必须长于 TcpConnection
    conn->setCloseCallback([this](const std::shared_ptr<TcpConnection>& c) mutable noexcept {
            this->removeConnection(c);
    });
    
    {
        std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
        _connection = conn;
    }
    
    conn->connectEstablished();
}

} // namespace KzNet