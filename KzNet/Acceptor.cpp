#include "Acceptor.h"
#include "EventLoop.h"
#include "KzLog/Logger.h"
#include "KzTimer/TimeStamp.h"

namespace KzNet {

void Acceptor::listen() noexcept {
    // 必须在 Loop 线程调用
    assert(_loop->isInLoopThread());

    _listening = true;
    _acceptSocket.listen();
    // 注册到 Epoll
    _acceptChannel.enableReading();
}

void Acceptor::handleRead() noexcept {
    // 必须在 Loop 线程调用
    assert(_loop->isInLoopThread());

    // 应对突发连接，避免每次 Epoll 返回只处理一个连接
    // 限制最大次数 (16)，防止恶意攻击导致 Acceptor 饿死 Loop 中的其他事件
    const int kMaxAcceptLoop = 16;
    int loopCount = 0;

    InetAddress peerAddr(0, false, false); // 初始化一个空地址
    while (loopCount < kMaxAcceptLoop) {
        int connfd = _acceptSocket.accept(&peerAddr);

        if (connfd >= 0) {
            // 成功接受连接
            if (_newConnectionCallback) _newConnectionCallback(connfd, peerAddr);
            else ::close(connfd);
        }
        else {
            // 正常情况：没有更多连接了
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            // 信号中断：重试
            if (errno == EINTR) {
                continue;
            }

            // === 处理 EMFILE ===
            // 如果进程 FD 耗尽，accept 会返回 EMFILE。
            // 在 LT 模式下，因为连接还在内核队列中没被取走，Epoll 会立刻再次触发 EPOLLIN。
            // 这会导致 Busy Loop (CPU 100%)。
            // 解决方案：
            // 关闭预留的 idleFd (腾出一个坑位)。
            // accept 这个连接 (消耗掉内核队列中的请求)。
            // 立即 close 这个连接。
            // 重新打开 idleFd (为下一次 EMFILE 做准备)。
            if (errno == EMFILE) {
                LOG_ERROR << "Acceptor: EMFILE reached! closing incoming connection.";
                if (_idleFd >= 0) {
                    ::close(_idleFd);
                    // 即使是处理垃圾连接，也必须使用非阻塞的 accept4
                    // 原因：如果在这短短的几微秒内，客户端主动发送了 RST 放弃连接，
                    // ::accept 可能会阻塞（因为内核队列里的连接消失了），从而卡死整个 EventLoop
                    int idle_conn = ::accept4(_acceptSocket.fd(), NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    // 如果 accept4 失败（比如客户端在这一瞬间发送了 RST 报文），idle_conn 会返回 -1
                    if (idle_conn >= 0) ::close(idle_conn);
                    _idleFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                } else {
                    // 防御：连 idleFd 都没了，说明系统彻底枯竭。
                    // 必须暂时注销 EPOLLIN 事件，否则会死循环。
                    // 设定一个定时器，100ms 后再重新 enableReading()。
                    _acceptChannel.disableReading();
                    _loop->addTimerDetached([this] mutable noexcept { _acceptChannel.enableReading(); },
                         KzTimer::TimeStamp::now().addSeconds(1));
                }
                break;
            }

            // 其他错误 (ECONNABORTED 等)
            LOG_SYSERR << "Acceptor::accept";
            break;
        }
        loopCount++;
    }
}

} // namespace KzNet
