#pragma once

#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/types.h>
#include <cstring>
#include <string_view>
#include <cassert>
#include <cerrno>
#include "InetAddress.h"
#include "KzLog/Logger.h"
#include "KzAlgorithm/Jeaiii.h"

namespace KzNet {

/**
 * @brief  Socket 封装
 * * 核心职责：提供对 TCP 协议栈的精细化控制。
 * * 覆盖范围：延迟控制、缓冲区控制、连接关闭行为、拥塞控制等。
 */
class Socket {
public:
    inline explicit Socket(int sockfd) noexcept : _sockfd(sockfd) {}
    ~Socket() {
        if (_sockfd >= 0) [[likely]] {
            ::close(_sockfd);
        }
    }

    // 不允许拷贝
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Move 语义
    Socket(Socket&& other) noexcept : _sockfd(other._sockfd) {
        other._sockfd = -1;
    }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) [[likely]] {
            if (_sockfd >= 0) ::close(_sockfd);
            _sockfd = other._sockfd;
            other._sockfd = -1;
        }
        return *this;
    }

    // 获取底层文件描述符
    int fd() const noexcept { return _sockfd; }

    // === 基础生命周期 ===
    void bindAddress(const InetAddress& addr) noexcept;
    void listen() noexcept;
    int accept(InetAddress* peeraddr) noexcept;
    void shutdownWrite() noexcept;


    // === 核心性能选项 ===
    /**
     * @brief TCP_NODELAY (禁用 Nagle 算法)
     * * 场景：RPC、游戏、即时通讯。
     * * 作用：有数据立即发送，不等待凑满一个 MSS。降低延迟，但可能增加小包数量。
     */
    void setTcpNoDelay(bool on) noexcept;

    /**
     * @brief TCP_CORK (塞子)
     * * 场景：文件服务器 (sendfile)、HTTP 分块传输。
     * * 作用：强制阻塞发送，直到凑满一个 MSS 或被显式取消。
     * * 注意：与 TCP_NODELAY 互斥，通常用于优化吞吐量。
     */
    void setTcpCork(bool on) noexcept;

    /**
     * @brief TCP_QUICKACK (快速确认)
     * * 场景：极低延迟场景。
     * * 作用：禁止延迟确认 (Delayed ACK)，收到数据立刻回 ACK。
     * * 注意：这不是永久标志，每次 recv 后可能需要重新设置。
     */
    void setTcpQuickAck(bool on) noexcept;


    // === 缓冲区控制 (Flow Control) ===
    /**
     * @brief SO_SNDBUF / SO_RCVBUF (发送/接收缓冲区大小)
     * * 警告：设置此选项会禁用 Linux 的 TCP 窗口自动调优 (Auto-Tuning)。
     * * 场景：
     *   1. 明确知道带宽延迟积 (BDP) 的专线网络。
     *   2. 限制单连接内存消耗，防止 OOM。
     */
    void setSendBuffSize(int size) noexcept;
    void setRecvBuffSize(int size) noexcept;


    // === 连接行为控制 ===
    /**
     * @brief SO_LINGER (优雅关闭 vs 强制复位)
     * * on=true, timeout=0: close() 立即返回，发送 RST (丢弃缓冲区数据)。用于快速释放端口。
     * * on=true, timeout>0: close() 尝试发送剩余数据，最多等待 timeout 秒。
     * * on=false: 默认行为，后台尽力发送。
     */
    void setLinger(bool on, int timeout) noexcept;

    /**
     * @brief SO_REUSEADDR (地址复用)
     * * 必须在 bind 前设置。允许重用 TIME_WAIT 状态的地址。
     */
    void setReuseAddr(bool on) noexcept;

    /**
     * @brief SO_REUSEPORT (端口复用)
     * * 允许内核层面的负载均衡 (多进程监听同一端口)。
     */
    void setReusePort(bool on) noexcept;

    /**
     * @brief SO_KEEPALIVE (内核级保活)
     * * 注意：默认间隔通常是 2小时，通常需要配合 TCP_KEEPIDLE 等选项修改，
     *   或者使用应用层心跳 (TimerQueue) 代替。
     */
    void setKeepAlive(bool on) noexcept;

    /**
     * @brief TCP_FASTOPEN (TFO)
     * * 场景：短连接优化。
     * * 作用：在三次握手期间交换数据，减少 1 个 RTT。
     */
    void setTcpFastOpen(int qlen) noexcept;


    // === 状态监控 ===
    // 获取 TCP 详细状态 (RTT, cwnd, ssthresh 等)
    bool getTcpInfo(struct tcp_info*) const noexcept;
    /**
     * @brief 获取格式化后的 TCP 状态字符串
     * * 格式: "state=1 ca_state=0 retrans=0 rtt=2000 cwnd=10 ..."
     * @param buf 用户提供的缓冲区 (建议 >= 256 bytes)
     * @param len 缓冲区长度
     * @return bool 是否成功
     */
    bool getTcpInfoString(char* buf, size_t len) const noexcept;
    
    // 获取 Socket 错误状态 (用于非阻塞 connect 回调判断成功与否)
    static int getSocketError(int sockfd) noexcept {
        int optval;
        socklen_t len = sizeof(optval);
        if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &len) < 0) [[unlikely]] {
            return errno;
        }
        return optval;
    }
    int getSocketError() noexcept {
        return getSocketError(_sockfd);
    }

    static bool isSelfConnect(int sockfd) noexcept {
        InetAddress local = getLocalAddr(sockfd);
        InetAddress peer = getPeerAddr(sockfd);
        const sockaddr* localaddr = local.getSockAddr();
        const sockaddr* peeraddr = peer.getSockAddr();
    
        if (localaddr->sa_family == AF_INET) {
            const struct sockaddr_in* laddr4 = reinterpret_cast<const struct sockaddr_in*>(localaddr);
            const struct sockaddr_in* paddr4 = reinterpret_cast<const struct sockaddr_in*>(peeraddr);
            return laddr4->sin_port == paddr4->sin_port &&
                laddr4->sin_addr.s_addr == paddr4->sin_addr.s_addr;
        } 
        else {
            const struct sockaddr_in6* laddr6 = reinterpret_cast<const struct sockaddr_in6*>(localaddr);
            const struct sockaddr_in6* paddr6 = reinterpret_cast<const struct sockaddr_in6*>(peeraddr);
            return laddr6->sin6_port == paddr6->sin6_port &&
               ::memcmp(&(laddr6->sin6_addr), &(paddr6->sin6_addr), sizeof(laddr6->sin6_addr)) == 0;
        }
    }
    
    // 辅助：获取地址
    static InetAddress getLocalAddr(int sockfd) noexcept {
        struct sockaddr_storage addr{};
        socklen_t addrlen = sizeof(addr);
        if (::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) < 0) [[unlikely]] {
            LOG_SYSERR << "Socket::getLocalAddr";
        }
        if (addr.ss_family == AF_INET) return InetAddress(*reinterpret_cast<struct sockaddr_in*>(&addr));
        return InetAddress(*reinterpret_cast<struct sockaddr_in6*>(&addr));
    }

    static InetAddress getPeerAddr(int sockfd) noexcept {
        struct sockaddr_storage addr{};
        socklen_t addrlen = sizeof(addr);
        if (::getpeername(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) < 0) [[unlikely]] {
            LOG_SYSERR << "Socket::getPeerAddr";
        }
        if (addr.ss_family == AF_INET) return InetAddress(*reinterpret_cast<struct sockaddr_in*>(&addr));
        return InetAddress(*reinterpret_cast<struct sockaddr_in6*>(&addr));
    }

    // === 静态工厂 ===
    static int createNonblockingOrDie(sa_family_t family) noexcept {
        // 直接在创建时设置 NONBLOCK 和 CLOEXEC
        int sockfd = ::socket(family, 
                          SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 
                          IPPROTO_TCP);
        if (sockfd < 0) [[unlikely]] {
            LOG_FATAL << "Socket::createNonblockingOrDie failed";
        }
        return sockfd;
    }

private:
    int _sockfd;
};

inline void Socket::bindAddress(const InetAddress& addr) noexcept {
    if (::bind(_sockfd, addr.getSockAddr(), addr.static_addr_len()) < 0) [[unlikely]] {
        LOG_FATAL << "Socket::bindAddress failed: "  << addr.toIpPortString();
    }
}

inline void Socket::listen() noexcept {
    if (::listen(_sockfd, SOMAXCONN) < 0) [[unlikely]] {
        LOG_FATAL << "Socket::bindAddress failed";
    }
}

inline int Socket::accept(InetAddress* peeraddr) noexcept {
    struct sockaddr_storage addr{}; // 使用足够大且安全的 storage
    socklen_t len = sizeof(addr);
    int connfd = ::accept4(_sockfd,
                           reinterpret_cast<struct sockaddr*>(&addr),
                           &len,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0) [[likely]] {
        if (addr.ss_family == AF_INET) {
            peeraddr->setSockAddr(*reinterpret_cast<struct sockaddr_in*>(&addr));
        } else if (addr.ss_family == AF_INET6) {
            peeraddr->setSockAddr(*reinterpret_cast<struct sockaddr_in6*>(&addr));
        }
    }
    else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_SYSERR << "Socket::accept";
        }
        // 上层 Acceptor 应该捕获这个情况并暂停 accept
        if (errno == EMFILE) {
            // 遇到 EMFILE 是灾难，通常需要预留一个 idle fd 来优雅关闭
            // 或者直接报错
            // 这里选择后者，交给Acceptor处理
            LOG_ERROR << "Socket::accept EMFILE - Process file descriptor limit reached!";
        }
    }
    return connfd;
}

inline void Socket::shutdownWrite() noexcept {
    if (::shutdown(_sockfd, SHUT_WR) < 0) [[unlikely]] {
        LOG_SYSERR << "Socket::shutdownWrite";
    }
}

inline void Socket::setTcpNoDelay(bool on) noexcept {
    int optval = on ? 1 : 0;
    if (::setsockopt(_sockfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0) [[unlikely]] {
        LOG_SYSERR << "setsockopt TCP_NODELAY failed";
    }
}

inline void Socket::setTcpCork(bool on) noexcept {
    int optval = on ? 1 : 0;
    if (::setsockopt(_sockfd, IPPROTO_TCP, TCP_CORK, &optval, sizeof(optval)) < 0) [[unlikely]] {
        LOG_SYSERR << "setsockopt TCP_CORK failed";
    }
}

inline void Socket::setTcpQuickAck(bool on) noexcept {
    int optval = on ? 1 : 0;
    if (::setsockopt(_sockfd, IPPROTO_TCP, TCP_QUICKACK, &optval, sizeof(optval)) < 0) [[unlikely]] {
        LOG_SYSERR << "setsockopt TCP_QUICKACK failed";
    }
}

inline void Socket::setSendBuffSize(int size) noexcept {
    if (::setsockopt(_sockfd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) [[unlikely]] {
        LOG_SYSERR << "setsockopt SO_SNDBUF failed";
    }
}

inline void Socket::setRecvBuffSize(int size) noexcept {
    if (::setsockopt(_sockfd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) [[unlikely]] {
        LOG_SYSERR << "setsockopt SO_RCVBUF failed";
    }
}

inline void Socket::setLinger(bool on, int timeout) noexcept {
    struct linger ling;
    ling.l_onoff = on ? 1 : 0;
    ling.l_linger = timeout;
    if (::setsockopt(_sockfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling)) < 0) [[unlikely]] {
        LOG_SYSERR << "setsockopt SO_LINGER failed";
    }
}

inline void Socket::setReuseAddr(bool on) noexcept {
    int optval = on ? 1 : 0;
    if (::setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) [[unlikely]] {
        LOG_SYSERR << "setsockopt SO_REUSEADDR failed";
    }
}

inline void Socket::setReusePort(bool on) noexcept {
#ifdef SO_REUSEPORT
    int optval = on ? 1 : 0;
    if (::setsockopt(_sockfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0) [[unlikely]] {
        LOG_SYSERR << "setsockopt SO_REUSEPORT failed";
    }
#else
    if (on) LOG_WARN << "SO_REUSEPORT is not supported on this platform";
#endif
}

inline void Socket::setKeepAlive(bool on) noexcept {
    int optval = on ? 1 : 0;
    if (::setsockopt(_sockfd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) [[unlikely]] {
        LOG_SYSERR << "setsockopt SO_KEEPALIVE failed";
    }
}

inline void Socket::setTcpFastOpen(int qlen) noexcept {
#ifdef TCP_FASTOPEN
    // qlen 是 TFO 队列长度，通常设为 5 或更大
    if (::setsockopt(_sockfd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) < 0) [[unlikely]] {
        // TFO 需要内核支持 (net.ipv4.tcp_fastopen)，失败不一定是致命的
        LOG_WARN << "setsockopt TCP_FASTOPEN failed (check sysctl net.ipv4.tcp_fastopen)";
    }
#else
    LOG_WARN << "TCP_FASTOPEN is not supported on this platform";
#endif
}

inline bool Socket::getTcpInfo(struct tcp_info* tcpi) const noexcept {
    socklen_t len = sizeof(*tcpi);
    return ::getsockopt(_sockfd, SOL_TCP, TCP_INFO, tcpi, &len) == 0;
}

} // namespace KzNet