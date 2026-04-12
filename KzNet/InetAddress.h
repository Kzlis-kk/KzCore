#pragma once

#include <netinet/in.h>
#include <string_view>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <cstddef>
#include "IpParser.h"
#include "KzSTL/KzString.h"
#include "KzLog/Logger.h"
#include "KzAlgorithm/Jeaiii.h"


namespace KzNet {
// 预计算网络字节序常量
// INADDR_ANY (0)
inline constexpr uint32_t kInAddrAny = INADDR_ANY; 
    
// INADDR_LOOPBACK (127.0.0.1) -> Network Byte Order
inline const uint32_t kInAddrLoopback = ::htonl(INADDR_LOOPBACK);

/**
 * @brief 网络地址封装 (IPv4 & IPv6)
 * * 核心特性：
 * 1. 可拷贝，可赋值，内存布局紧凑 (Union)。
 * 2. 构造和格式化过程尽可能避免堆分配。
 * 3. 透明支持 IPv6。
 * 4. 强制小端。
 */    
class InetAddress {

public:
    /**
     * @brief 默认构造 (监听地址)
     * @param port 端口号 (主机字节序)
     * @param loopbackOnly true: 127.0.0.1 / ::1; false: 0.0.0.0 / ::
     * @param ipv6 是否使用 IPv6
     */
    InetAddress(uint16_t port, bool loopbackOnly, bool ipv6) noexcept
    : _addr6{}
{
    if (ipv6) {
        _addr6.sin6_family = AF_INET6;
        _addr6.sin6_addr = loopbackOnly ? in6addr_loopback : in6addr_any;
        _addr6.sin6_port = ::htons(port);
    } else {
        _addr.sin_family = AF_INET;
        _addr.sin_addr.s_addr = loopbackOnly ? kInAddrLoopback : kInAddrAny;
        _addr.sin_port = ::htons(port);
    }
}

    /**
     * @brief 零拷贝构造 (使用 std::string_view )
     * * 性能：直接解析内存，无拷贝。
     */
    InetAddress(std::string_view ip, uint16_t port, bool ipv6) noexcept
    : _addr6{}
{
    // 启发式判断：如果包含 ':'，就是 IPv6
    if (ipv6 || ip.find(':') != std::string_view::npos) {
        _addr6.sin6_family = AF_INET6;
        _addr6.sin6_port = ::htons(port);
        if (!IpParser::parseIPv6(ip, &_addr6.sin6_addr)) [[unlikely]] {
            LOG_SYSERR << "InetAddress parse IPv6 failed: " << ip;
        }
    } else {
        _addr.sin_family = AF_INET;
        _addr.sin_port = ::htons(port);
        if (!IpParser::parseIPv4(ip, &_addr.sin_addr)) [[unlikely]] {
            LOG_SYSERR << "InetAddress parse IPv4 failed: " << ip;
        }
    }
}

    // 内核结构构造
    inline explicit InetAddress(const struct sockaddr_in& addr) noexcept : _addr(addr) {}
    inline explicit InetAddress(const struct sockaddr_in6& addr) noexcept : _addr6(addr) {}
    

    // 格式化接口
    // 类型双关，sockaddr_in 和 sockaddr_in6 是标准布局，而且首位成员都是地址族
    // 严格来说，C++中 union 只能访问当前存储的一个成员，不然就是 UB，但是只要断言偏移值为 0 也是能够接受的
    // 如果要保证严格安全，无论是使用 额外的 bool 还是std::varint<>都会有开销，所以暂定为此 
    sa_family_t family() const noexcept { return _addr.sin_family; }

    /**
     * @brief 零分配格式化 IP:Port
     * @param buf 用户提供的栈缓冲区 (建议 >= 64 字节)
     * @return 写入的长度
     */
    size_t toIpPort(char* buf, size_t size) const noexcept;

    KzSTL::KzString toIpPortString() const noexcept {
        char buf[64];
        size_t len = toIpPort(buf, sizeof(buf));
        return KzSTL::KzString(buf, len);
    }

    /**
     * @brief 格式化 IP
     */
    size_t toIp(char* buf, size_t size) const noexcept {
        if (family() == AF_INET6) {
            ::inet_ntop(AF_INET6, &_addr6.sin6_addr, buf, static_cast<socklen_t>(size));
        } else {
            ::inet_ntop(AF_INET, &_addr.sin_addr, buf, static_cast<socklen_t>(size));
        }
        return buf ? std::strlen(buf) : 0;
    }
    
    // 获取端口 (主机字节序)
    uint16_t toPort() const noexcept {
        return ::ntohs(_addr.sin_port);
    }


    // 底层访问
    // 获取底层指针 (用于 bind/connect)
    const struct sockaddr* getSockAddr() const noexcept {
        if (family() == AF_INET6) {
            return reinterpret_cast<const struct sockaddr*>(&_addr6);
        } else {
            return reinterpret_cast<const struct sockaddr*>(&_addr);
        }
    }

    void setSockAddr(const struct sockaddr_in& addr) noexcept { _addr = addr; }
    void setSockAddr(const struct sockaddr_in6& addr6) noexcept { _addr6 = addr6; }

    // 获取当前地址类型的结构体大小 (用于 bind/connect/accept)
    socklen_t static_addr_len() const noexcept {
        return family() == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    }

private:
    // 使用 Union 实现双栈支持，内存占用固定为 sizeof(sockaddr_in6) (28 bytes)
    union {
        struct sockaddr_in _addr;
        struct sockaddr_in6 _addr6;
    };
};

// 静态断言：确保 Union 布局符合预期
static_assert(offsetof(sockaddr_in, sin_family) == offsetof(sockaddr_in6, sin6_family),
    "Family offset mismatch");
static_assert(sizeof(InetAddress) == sizeof(struct sockaddr_in6),
    "InetAddress size mismatch");

inline size_t InetAddress::toIpPort(char* buf, size_t size) const noexcept {
    if (size < 64) [[unlikely]] return 0;

    size_t len = 0;
    uint16_t port = 0;

    if (family() == AF_INET6) {
        // IPv6 格式: [2001:db8::1]:8080
        buf[0] = '[';
        // inet_ntop 性能足够好，且 IPv6 格式化复杂，暂不手写
        ::inet_ntop(AF_INET6, &_addr6.sin6_addr, buf + 1, static_cast<socklen_t>(size - 1));
        len = std::strlen(buf);
        buf[len++] = ']';
        buf[len++] = ':';
        port = ::ntohs(_addr6.sin6_port);
    } else {
        // IPv4 格式: 127.0.0.1:8080
        ::inet_ntop(AF_INET, &_addr.sin_addr, buf, static_cast<socklen_t>(size));
        len = std::strlen(buf);
        buf[len++] = ':';
        port = ::ntohs(_addr.sin_port);
    }

    // 使用 Jeaiii 极速转换端口号 (零拷贝，无 snprintf)
    char* end = KzAlgorithm::Jeaiii::to_chars(buf + len, port);
    *end = '\0'; 
    
    return static_cast<size_t>(end - buf);
}

} // namespace KzNet