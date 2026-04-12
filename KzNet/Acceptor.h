#pragma once

#include "Channel.h"
#include "Socket.h"
#include "InetAddress.h"
#include "KzSTL/Function.h"
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

namespace KzNet {
    
class EventLoop;

/**
 * @brief TCP 连接接受器
 * 
 * * 核心职责：
 *   1. 封装监听 Socket (RAII)。
 *   2. 监听 EPOLLIN 事件，执行 accept。
 *   3. 处理文件描述符耗尽 (EMFILE) 的极端情况。
 * 
 *   只运行在主 IO 线程。
 */
class Acceptor {
public:
    // 新连接回调：传递 connfd 和 对端地址
    using NewConnectionCallback = KzSTL::Function<void(int sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddress, bool reuseport) noexcept;
    ~Acceptor() {
        _acceptChannel.disableAll();
        _acceptChannel.remove();
        ::close(_idleFd);
    }

    // 设置回调 (由 TcpServer 注册)
    void setNewConnectionCallback(const NewConnectionCallback& cb) noexcept { _newConnectionCallback = cb; }
    void setNewConnectionCallback(NewConnectionCallback&& cb) noexcept { _newConnectionCallback = std::move(cb); }

    // 开始监听
    void listen() noexcept;

    bool listening() noexcept { return _listening; }

private:
    // 处理读事件 (即新连接到达)
    void handleRead() noexcept;

    EventLoop* _loop;
    Socket _acceptSocket;
    Channel _acceptChannel;

    NewConnectionCallback _newConnectionCallback;
    bool _listening;
    // 占位文件描述符 (用于处理 EMFILE)
    int _idleFd;
};

inline Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddress, bool reuseport) noexcept
    : _loop(loop),
      _acceptSocket(Socket::createNonblockingOrDie(listenAddress.family())),
      _acceptChannel(loop, _acceptSocket.fd()),
      _listening(false),
      // 预留一个空闲 FD (打开 /dev/null)
      _idleFd(::open("/dev/null", O_RDONLY | O_CLOEXEC))
{
    assert(_idleFd >= 0);

    // 设置 Socket 选项
    _acceptSocket.setReuseAddr(true); // 允许重用 TIME_WAIT 地址
    _acceptSocket.setReusePort(reuseport); // 内核级负载均衡 (Linux 3.9+)

    // 绑定地址
    _acceptSocket.bindAddress(listenAddress);

    // 设置 Channel 回调
    // 注意：Channel::ReadCallback 需要接收 Timestamp，但 accept 逻辑通常不关心时间
    // 这里使用 lambda 适配
    _acceptChannel.setReadCallback([this](KzTimer::TimeStamp) mutable noexcept {
        this->handleRead();
    });
}

} // namespace KzNet